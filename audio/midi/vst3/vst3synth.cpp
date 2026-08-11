//=============================================================================
//  MuseScore
//  VST3 instrument synthesizer
//=============================================================================

#include "vst3synth.h"

#include <algorithm>
#include <set>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSettings>

#include "audio/midi/event.h"
#include "vst3gui.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"

namespace Ms {

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace VST3::Hosting;

namespace {

QString normalizedPath(const QString& path)
      {
      const QFileInfo info(path);
      QString result = info.canonicalFilePath();
      if (result.isEmpty())
            result = info.absoluteFilePath();
      return QDir::cleanPath(result);
      }

int stablePatchId(const QString& path, const QString& classId)
      {
      QByteArray key = (QDir::toNativeSeparators(path).toLower() + QLatin1Char('|') + classId.toLower()).toUtf8();
      const QByteArray hash = QCryptographicHash::hash(key, QCryptographicHash::Sha1);
      return ((static_cast<unsigned char>(hash[0]) << 16)
              | (static_cast<unsigned char>(hash[1]) << 8)
              | static_cast<unsigned char>(hash[2])) & 0x1fffff;
      }

} // namespace

Vst3Synth::Vst3Synth()
      {
      PluginContextFactory::instance().setPluginContext(&_hostApplication);
      _stateCache.setName(name());
      loadCustomDirectories();
      rescanPlugins();
      }

Vst3Synth::~Vst3Synth()
      {
      for (int channel = 0; channel < CHANNEL_COUNT; ++channel)
            std::atomic_store(&_instances[channel], std::shared_ptr<Vst3Plugin>());
      qDeleteAll(_patches);
      _patches.clear();
      PluginContextFactory::instance().setPluginContext(nullptr);
      }

void Vst3Synth::init(float sampleRate)
      {
      if (_sampleRate != sampleRate) {
            for (int channel = 0; channel < CHANNEL_COUNT; ++channel)
                  releaseChannel(channel);
            }
      Synthesizer::init(sampleRate);
      }

QStringList Vst3Synth::pluginPaths() const
      {
      QStringList paths;
      for (const std::string& path : Module::getModulePaths())
            paths.append(QString::fromStdString(path));

      for (const QString& customPath : _customDirectories) {
            QFileInfo customInfo(customPath);
            if (customInfo.suffix().compare("vst3", Qt::CaseInsensitive) == 0) {
                  paths.append(customInfo.absoluteFilePath());
                  continue;
                  }
            QDirIterator it(customPath, QStringList() << "*.vst3",
                            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                  paths.append(it.next());
            }

      for (QString& path : paths)
            path = normalizedPath(path);
      paths.removeAll(QString());
      paths.removeDuplicates();
      std::sort(paths.begin(), paths.end(), [](const QString& left, const QString& right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
            });
      return paths;
      }

bool Vst3Synth::isInstrument(const ClassInfo& classInfo) const
      {
      if (classInfo.category() != kVstAudioEffectClass)
            return false;
      for (const std::string& category : classInfo.subCategories()) {
            if (QString::fromStdString(category).compare("Instrument", Qt::CaseInsensitive) == 0)
                  return true;
            }
      return classInfo.subCategoriesString().find("Instrument") != std::string::npos;
      }

bool Vst3Synth::rescanPlugins()
      {
      QList<Vst3PluginDescriptor> descriptors;
      QStringList errors;

      for (const QString& path : pluginPaths()) {
            std::string error;
            Module::Ptr module;
            try {
                  module = Module::create(path.toStdString(), error);
                  }
            catch (const std::exception& exception) {
                  error = exception.what();
                  }
            catch (...) {
                  error = "unknown exception";
                  }

            if (!module) {
                  errors.append(QString("%1: %2").arg(path, QString::fromStdString(error)));
                  continue;
                  }

            for (const ClassInfo& classInfo : module->getFactory().classInfos()) {
                  if (!isInstrument(classInfo))
                        continue;

                  Vst3PluginDescriptor descriptor;
                  descriptor.path = path;
                  descriptor.name = QString::fromStdString(classInfo.name());
                  descriptor.vendor = QString::fromStdString(classInfo.vendor());
                  if (descriptor.vendor.isEmpty())
                        descriptor.vendor = QStringLiteral("Unknown vendor");
                  descriptor.classId = QString::fromStdString(classInfo.ID().toString());
                  descriptor.classInfo = classInfo;
                  descriptors.append(descriptor);
                  }
            }

      std::sort(descriptors.begin(), descriptors.end(), [](const Vst3PluginDescriptor& left,
                                                            const Vst3PluginDescriptor& right) {
            const int vendorCompare = left.vendor.compare(right.vendor, Qt::CaseInsensitive);
            if (vendorCompare != 0)
                  return vendorCompare < 0;
            const int nameCompare = left.name.compare(right.name, Qt::CaseInsensitive);
            if (nameCompare != 0)
                  return nameCompare < 0;
            return left.classId < right.classId;
            });

      std::set<int> usedPatchIds;
      for (Vst3PluginDescriptor& descriptor : descriptors) {
            int patchId = stablePatchId(descriptor.path, descriptor.classId);
            while (usedPatchIds.count(patchId))
                  patchId = (patchId + 1) & 0x1fffff;
            usedPatchIds.insert(patchId);
            descriptor.bank = patchId >> 7;
            descriptor.program = patchId & 0x7f;
            }

      {
      QMutexLocker locker(&_metadataMutex);
      _descriptors = descriptors;
      _scanErrors = errors;
      rebuildPatchList();
      }
      return !descriptors.isEmpty();
      }

void Vst3Synth::rebuildPatchList()
      {
      qDeleteAll(_patches);
      _patches.clear();
      int sfid = 10000;
      for (const Vst3PluginDescriptor& descriptor : _descriptors) {
            auto* patch = new MidiPatch;
            patch->drum = false;
            patch->synti = name();
            patch->bank = descriptor.bank;
            patch->prog = descriptor.program;
            patch->sfid = sfid++;
            patch->name = descriptor.displayName();
            _patches.append(patch);
            }
      }

const Vst3PluginDescriptor* Vst3Synth::descriptorForPatch(int bank, int program) const
      {
      for (const Vst3PluginDescriptor& descriptor : _descriptors) {
            if (descriptor.bank == bank && descriptor.program == program)
                  return &descriptor;
            }
      return nullptr;
      }

bool Vst3Synth::prepareChannel(int channel, int bank, int program)
      {
      if (channel < 0 || channel >= CHANNEL_COUNT)
            return false;

      const Vst3PluginDescriptor* descriptor = descriptorForPatch(bank, program);
      if (!descriptor) {
            releaseChannel(channel);
            return false;
            }

      std::shared_ptr<Vst3Plugin> current = std::atomic_load(&_instances[channel]);
      if (current && current->descriptor().path == descriptor->path
          && current->descriptor().classId == descriptor->classId)
            return true;

      const Vst3PluginState* restoreState = nullptr;
      auto saved = _savedInstances.constFind(channel);
      if (saved != _savedInstances.constEnd()
          && saved->path == descriptor->path && saved->classId == descriptor->classId)
            restoreState = &saved->state;

      auto plugin = std::make_shared<Vst3Plugin>(*descriptor);
      if (!plugin->initialize(_sampleRate, restoreState)) {
            qWarning() << "Unable to prepare VST3 instrument" << descriptor->displayName();
            return false;
            }

      std::atomic_store(&_instances[channel], plugin);
      setActive(true);
      return true;
      }

void Vst3Synth::releaseChannel(int channel)
      {
      if (channel >= 0 && channel < CHANNEL_COUNT)
            std::atomic_store(&_instances[channel], std::shared_ptr<Vst3Plugin>());
      }

void Vst3Synth::process(unsigned frames, float* output, float*, float*)
      {
      for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
            std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
            if (plugin)
                  plugin->process(frames, output);
            }
      }

void Vst3Synth::play(const PlayEvent& event)
      {
      const int channel = event.channel();
      if (channel < 0 || channel >= CHANNEL_COUNT)
            return;
      std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
      if (plugin)
            plugin->handleEvent(event);
      }

void Vst3Synth::setPlaybackState(bool playing, double tempo)
      {
      for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
            std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
            if (plugin)
                  plugin->setPlaybackState(playing, tempo);
            }
      }

bool Vst3Synth::hasEditor(int channel) const
      {
      if (channel < 0 || channel >= CHANNEL_COUNT)
            return false;
      std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
      return plugin && plugin->hasEditor();
      }

bool Vst3Synth::openEditor(int channel, QWidget* parent)
      {
      if (channel < 0 || channel >= CHANNEL_COUNT)
            return false;
      std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
      return plugin && plugin->openEditor(parent);
      }

void Vst3Synth::allNotesOff(int channel)
      {
      if (channel == -1) {
            for (int currentChannel = 0; currentChannel < CHANNEL_COUNT; ++currentChannel) {
                  std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[currentChannel]);
                  if (plugin)
                        plugin->allNotesOff();
                  }
            return;
            }

      if (channel < 0 || channel >= CHANNEL_COUNT)
            return;
      std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
      if (plugin)
            plugin->allNotesOff();
      }

void Vst3Synth::allSoundsOff(int channel)
      {
      allNotesOff(channel);
      }

void Vst3Synth::reset()
      {
      for (int channel = 0; channel < CHANNEL_COUNT; ++channel)
            allNotesOff(channel);
      Synthesizer::reset();
      }

void Vst3Synth::loadCustomDirectories()
      {
      QSettings settings;
      _customDirectories = settings.value("VST3Host/directories").toStringList();
      for (QString& directory : _customDirectories)
            directory = normalizedPath(directory);
      _customDirectories.removeAll(QString());
      _customDirectories.removeDuplicates();
      }

void Vst3Synth::saveCustomDirectories() const
      {
      QSettings settings;
      settings.setValue("VST3Host/directories", _customDirectories);
      }

bool Vst3Synth::loadSoundFonts(const QStringList& paths)
      {
      _customDirectories.clear();
      for (const QString& path : paths) {
            const QString normalized = normalizedPath(path);
            if (!normalized.isEmpty())
                  _customDirectories.append(normalized);
            }
      _customDirectories.removeDuplicates();
      saveCustomDirectories();
      return rescanPlugins();
      }

bool Vst3Synth::addSoundFont(const QString& path)
      {
      return addCustomDirectory(path);
      }

bool Vst3Synth::removeSoundFont(const QString& path)
      {
      return removeCustomDirectory(path);
      }

std::vector<SoundFontInfo> Vst3Synth::soundFontsInfo() const
      {
      std::vector<SoundFontInfo> result;
      QMutexLocker locker(&_metadataMutex);
      result.reserve(_descriptors.size());
      for (const Vst3PluginDescriptor& descriptor : _descriptors)
            result.emplace_back(descriptor.path, descriptor.displayName());
      return result;
      }

bool Vst3Synth::addCustomDirectory(const QString& path)
      {
      const QString normalized = normalizedPath(path);
      if (normalized.isEmpty() || !QFileInfo(normalized).exists())
            return false;
      if (!_customDirectories.contains(normalized, Qt::CaseInsensitive))
            _customDirectories.append(normalized);
      saveCustomDirectories();
      rescanPlugins();
      return true;
      }

bool Vst3Synth::removeCustomDirectory(const QString& path)
      {
      const QString normalized = normalizedPath(path);
      for (int index = 0; index < _customDirectories.size(); ++index) {
            if (_customDirectories[index].compare(normalized, Qt::CaseInsensitive) == 0) {
                  _customDirectories.removeAt(index);
                  saveCustomDirectories();
                  rescanPlugins();
                  return true;
                  }
            }
      return false;
      }

QStringList Vst3Synth::customDirectories() const
      {
      return _customDirectories;
      }

QList<Vst3PluginDescriptor> Vst3Synth::descriptors() const
      {
      QMutexLocker locker(&_metadataMutex);
      return _descriptors;
      }

QStringList Vst3Synth::scanErrors() const
      {
      QMutexLocker locker(&_metadataMutex);
      return _scanErrors;
      }

int Vst3Synth::activeChannel(int bank, int program) const
      {
      for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
            std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
            if (plugin && plugin->descriptor().bank == bank
                && plugin->descriptor().program == program)
                  return channel;
            }
      return -1;
      }

void Vst3Synth::prepareState()
      {
      SynthesizerGroup group;
      group.setName(name());
      for (const QString& directory : _customDirectories)
            group.push_back(IdValue(0, directory));

      for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
            std::shared_ptr<Vst3Plugin> plugin = std::atomic_load(&_instances[channel]);
            if (!plugin)
                  continue;

            const Vst3PluginState pluginState = plugin->captureState();
            QJsonObject object;
            object["channel"] = channel;
            object["bank"] = plugin->descriptor().bank;
            object["program"] = plugin->descriptor().program;
            object["path"] = plugin->descriptor().path;
            object["classId"] = plugin->descriptor().classId;
            object["component"] = QString::fromLatin1(pluginState.component.toBase64());
            object["controller"] = QString::fromLatin1(pluginState.controller.toBase64());
            group.push_back(IdValue(1, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))));
            }
      _stateCache = group;
      }

SynthesizerGroup Vst3Synth::state() const
      {
      if (_stateCache.empty()) {
            SynthesizerGroup group;
            group.setName(name());
            for (const QString& directory : _customDirectories)
                  group.push_back(IdValue(0, directory));
            return group;
            }
      return _stateCache;
      }

bool Vst3Synth::setState(const SynthesizerGroup& group)
      {
      QStringList directories;
      QMap<int, SavedInstance> savedInstances;
      for (const IdValue& value : group) {
            if (value.id == 0) {
                  directories.append(value.data);
                  continue;
                  }
            if (value.id != 1)
                  continue;

            const QJsonDocument document = QJsonDocument::fromJson(value.data.toUtf8());
            if (!document.isObject())
                  continue;
            const QJsonObject object = document.object();
            const int channel = object["channel"].toInt(-1);
            if (channel < 0 || channel >= CHANNEL_COUNT)
                  continue;
            SavedInstance saved;
            saved.bank = object["bank"].toInt();
            saved.program = object["program"].toInt();
            saved.path = object["path"].toString();
            saved.classId = object["classId"].toString();
            saved.state.component = QByteArray::fromBase64(object["component"].toString().toLatin1());
            saved.state.controller = QByteArray::fromBase64(object["controller"].toString().toLatin1());
            savedInstances[channel] = saved;
            }

      if (!directories.isEmpty()) {
            _customDirectories = directories;
            for (QString& directory : _customDirectories)
                  directory = normalizedPath(directory);
            _customDirectories.removeAll(QString());
            _customDirectories.removeDuplicates();
            saveCustomDirectories();
            }
      _savedInstances = savedInstances;
      _stateCache = group;
      rescanPlugins();
      return true;
      }

SynthesizerGui* Vst3Synth::gui()
      {
      if (!_gui)
            _gui = new Vst3Gui(this);
      return _gui;
      }

Synthesizer* createVst3Synth()
      {
      return new Vst3Synth;
      }

} // namespace Ms
