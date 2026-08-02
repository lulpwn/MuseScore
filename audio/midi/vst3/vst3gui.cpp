//=============================================================================
//  MuseScore
//  VST3 instrument synthesizer user interface
//=============================================================================

#include "vst3gui.h"
#include "vst3synth.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace Ms {

Vst3Gui::Vst3Gui(Synthesizer* synth)
   : SynthesizerGui(synth)
      {
      auto* layout = new QVBoxLayout(this);
      layout->setContentsMargins(16, 14, 16, 14);
      layout->setSpacing(12);

      auto* title = new QLabel(tr("VST3 piano instrument"), this);
      QFont titleFont = title->font();
      titleFont.setBold(true);
      titleFont.setPointSize(titleFont.pointSize() + 2);
      title->setFont(titleFont);
      layout->addWidget(title);

      auto* explanation = new QLabel(tr(
         "Choose an installed instrument. MuseScore remembers this list and can route piano parts to it automatically."), this);
      explanation->setWordWrap(true);
      layout->addWidget(explanation);

      auto* instrumentGroup = new QGroupBox(tr("Instrument"), this);
      auto* instrumentLayout = new QVBoxLayout(instrumentGroup);
      instrumentLayout->setSpacing(8);

      auto* pluginLabel = new QLabel(tr("Available plug-ins"), instrumentGroup);
      instrumentLayout->addWidget(pluginLabel);

      auto* pluginRow = new QHBoxLayout;
      _plugins = new QComboBox(instrumentGroup);
      _plugins->setEditable(false);
      _plugins->setInsertPolicy(QComboBox::NoInsert);
      _plugins->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
      _plugins->setMinimumContentsLength(28);
      pluginRow->addWidget(_plugins, 1);

      auto* browseButton = new QPushButton(tr("Add plug-in..."), instrumentGroup);
      _scanButton = new QPushButton(tr("Rescan"), instrumentGroup);
      pluginRow->addWidget(browseButton);
      pluginRow->addWidget(_scanButton);
      instrumentLayout->addLayout(pluginRow);

      _pluginPath = new QLabel(instrumentGroup);
      _pluginPath->setWordWrap(true);
      _pluginPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
      _pluginPath->setStyleSheet("color: palette(mid);");
      instrumentLayout->addWidget(_pluginPath);

      _routeToPiano = new QCheckBox(tr("Use this instrument for piano playback"), instrumentGroup);
      _routeToPiano->setChecked(true);
      _routeToPiano->setToolTip(tr(
         "When loading the instrument, switch piano channels from FluidSynth to the VST3 patch."));
      instrumentLayout->addWidget(_routeToPiano);
      layout->addWidget(instrumentGroup);

      auto* performanceGroup = new QGroupBox(tr("Playback performance"), this);
      auto* performanceLayout = new QVBoxLayout(performanceGroup);
      auto* bufferRow = new QHBoxLayout;
      auto* bufferLabel = new QLabel(tr("Audio buffer"), performanceGroup);
      _audioBuffer = new QComboBox(performanceGroup);
      _audioBuffer->addItem(tr("Automatic (lowest latency)"), 0);
      _audioBuffer->addItem(tr("256 samples"), 256);
      _audioBuffer->addItem(tr("512 samples (recommended)"), 512);
      _audioBuffer->addItem(tr("1024 samples (maximum stability)"), 1024);
      bufferRow->addWidget(bufferLabel);
      bufferRow->addWidget(_audioBuffer, 1);
      performanceLayout->addLayout(bufferRow);
      auto* bufferHelp = new QLabel(tr(
         "A larger buffer reduces missed notes and stuttering, at the cost of more playback latency. "
         "Restart MuseScore after changing it."), performanceGroup);
      bufferHelp->setWordWrap(true);
      bufferHelp->setStyleSheet("color: palette(mid);");
      performanceLayout->addWidget(bufferHelp);
      layout->addWidget(performanceGroup);

      auto* buttonRow = new QHBoxLayout;
      _loadButton = new QPushButton(tr("Load selected"), this);
      _loadButton->setDefault(true);
      _editorButton = new QPushButton(tr("Open editor"), this);
      _unloadButton = new QPushButton(tr("Unload"), this);
      buttonRow->addWidget(_loadButton);
      buttonRow->addWidget(_editorButton);
      buttonRow->addWidget(_unloadButton);
      buttonRow->addStretch(1);
      layout->addLayout(buttonRow);

      _status = new QLabel(this);
      _message = new QLabel(this);
      _message->setWordWrap(true);
      _message->setStyleSheet("color: #b00020;");
      layout->addWidget(_status);
      layout->addWidget(_message);
      layout->addStretch(1);

      connect(_scanButton, SIGNAL(clicked()), SLOT(refreshPlugins()));
      connect(browseButton, SIGNAL(clicked()), SLOT(browsePlugin()));
      connect(_loadButton, SIGNAL(clicked()), SLOT(loadSelectedPlugin()));
      connect(_editorButton, SIGNAL(clicked()), SLOT(openEditor()));
      connect(_unloadButton, SIGNAL(clicked()), SLOT(unloadPlugin()));

      _serviceTimer = new QTimer(this);
      _serviceTimer->setInterval(30);
      connect(_serviceTimer, SIGNAL(timeout()), SLOT(servicePlugin()));
      _serviceTimer->start();

      loadCachedPlugins();
      connect(_plugins, SIGNAL(currentIndexChanged(int)), SLOT(selectedPluginChanged(int)));
      connect(_routeToPiano, SIGNAL(toggled(bool)), SLOT(routeOptionChanged(bool)));
      connect(_audioBuffer, SIGNAL(currentIndexChanged(int)), SLOT(audioBufferChanged(int)));
      updateControls();
      if (_plugins->count() == 0)
            refreshPlugins();
      }

Vst3Synth* Vst3Gui::vst3()
      {
      return static_cast<Vst3Synth*>(synthesizer());
      }

QString Vst3Gui::selectedPath() const
      {
      const int index = _plugins->currentIndex();
      return index >= 0 ? QDir::toNativeSeparators(_plugins->itemData(index).toString()) : QString();
      }

void Vst3Gui::addPluginPath(const QString& path, bool select)
      {
      if (path.isEmpty())
            return;
      const QString nativePath = QDir::toNativeSeparators(path);
      for (int i = 0; i < _plugins->count(); ++i) {
            if (QDir::cleanPath(_plugins->itemData(i).toString()).compare(
                   QDir::cleanPath(nativePath), Qt::CaseInsensitive) == 0) {
                  if (select)
                        _plugins->setCurrentIndex(i);
                  return;
                  }
            }
      const QString label = QFileInfo(nativePath).completeBaseName();
      _plugins->addItem(label.isEmpty() ? nativePath : label, nativePath);
      _plugins->setItemData(_plugins->count() - 1, nativePath, Qt::ToolTipRole);
      if (select)
            _plugins->setCurrentIndex(_plugins->count() - 1);
      }

void Vst3Gui::loadCachedPlugins()
      {
      QSettings settings;
      settings.beginGroup("VST3Host");
      const bool currentCatalog = settings.value("instrumentCatalogVersion", 0).toInt() == 1;
      const QStringList paths = currentCatalog ? settings.value("pluginPaths").toStringList() : QStringList();
      const QString lastPath = currentCatalog ? settings.value("lastPluginPath").toString() : QString();
      _routeToPiano->setChecked(settings.value("routePiano", true).toBool());
      const int bufferFrames = settings.value("audioBufferFrames", 512).toInt();
      settings.endGroup();

      int bufferIndex = _audioBuffer->findData(bufferFrames);
      if (bufferIndex < 0)
            bufferIndex = _audioBuffer->findData(512);
      _audioBuffer->setCurrentIndex(bufferIndex);

      for (const QString& path : paths) {
            if (QFileInfo::exists(path))
                  addPluginPath(path, false);
            }
      if (!lastPath.isEmpty() && QFileInfo::exists(lastPath))
            addPluginPath(lastPath, true);
      if (_plugins->currentIndex() < 0 && _plugins->count() > 0)
            _plugins->setCurrentIndex(0);
      updateSelectionDetails();
      }

void Vst3Gui::saveCachedPlugins() const
      {
      QStringList paths;
      for (int i = 0; i < _plugins->count(); ++i) {
            const QString path = _plugins->itemData(i).toString();
            if (!path.isEmpty())
                  paths.append(path);
            }

      QSettings settings;
      settings.beginGroup("VST3Host");
      settings.setValue("instrumentCatalogVersion", 1);
      settings.setValue("pluginPaths", paths);
      settings.setValue("lastPluginPath", selectedPath());
      settings.setValue("routePiano", _routeToPiano->isChecked());
      settings.setValue("audioBufferFrames", _audioBuffer->currentData().toInt());
      settings.endGroup();
      }

void Vst3Gui::updateSelectionDetails()
      {
      const QString path = selectedPath();
      _pluginPath->setText(path.isEmpty() ? tr("No plug-in selected") : tr("Location: %1").arg(path));
      _loadButton->setEnabled(!path.isEmpty());
      }

void Vst3Gui::refreshPlugins()
      {
      const QString previous = selectedPath();
      _scanButton->setEnabled(false);
      _scanButton->setText(tr("Scanning..."));
      QApplication::setOverrideCursor(Qt::WaitCursor);
      const QStringList paths = vst3()->availablePlugins();
      QApplication::restoreOverrideCursor();
      _scanButton->setText(tr("Rescan"));
      _scanButton->setEnabled(true);

      _plugins->clear();
      for (const QString& path : paths)
            addPluginPath(path, false);
      for (int i = 0; i < _plugins->count(); ++i) {
            if (QDir::cleanPath(_plugins->itemData(i).toString()).compare(
                   QDir::cleanPath(previous), Qt::CaseInsensitive) == 0) {
                  _plugins->setCurrentIndex(i);
                  break;
                  }
            }

      if (_plugins->currentIndex() < 0 && _plugins->count() > 0)
            _plugins->setCurrentIndex(0);
      updateSelectionDetails();
      saveCachedPlugins();

      if (_plugins->count() == 0)
            _message->setText(tr("No VST3 plug-ins were found in the standard Windows VST3 folders."));
      else
            _message->clear();
      }

void Vst3Gui::browsePlugin()
      {
      QString start = selectedPath();
      if (start.isEmpty())
            start = QStringLiteral("C:/Program Files/Common Files/VST3");
      else if (QFileInfo(start).isDir())
            start = QFileInfo(start).absolutePath();
      const QString path = QFileDialog::getExistingDirectory(
         this, tr("Choose a .vst3 instrument bundle"), start, QFileDialog::ShowDirsOnly);
      if (!path.isEmpty()) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool instrument = vst3()->isInstrumentPlugin(path);
            QApplication::restoreOverrideCursor();
            if (!instrument) {
                  QMessageBox::warning(this, tr("MuseScore"),
                                       tr("That VST3 bundle does not contain an instrument."));
                  return;
                  }
            addPluginPath(path, true);
            updateSelectionDetails();
            saveCachedPlugins();
            }
      }

void Vst3Gui::loadSelectedPlugin()
      {
      const QString path = selectedPath();
      if (path.isEmpty()) {
            QMessageBox::information(this, tr("MuseScore"), tr("Choose a VST3 instrument first."));
            return;
            }

      QApplication::setOverrideCursor(Qt::WaitCursor);
      const bool loaded = vst3()->loadPlugin(path);
      QApplication::restoreOverrideCursor();
      updateControls();

      if (!loaded) {
            QMessageBox::warning(this, tr("MuseScore"),
                                 tr("The VST3 instrument could not be loaded.\n\n%1").arg(vst3()->lastError()));
            return;
            }

      emit valueChanged();
      emit sfChanged();
      saveCachedPlugins();
      if (_routeToPiano->isChecked())
            emit requestPatchRouting(QStringLiteral("VST3"));
      }

void Vst3Gui::openEditor()
      {
      if (!vst3()->showEditor(this))
            QMessageBox::warning(this, tr("MuseScore"),
                                 tr("The plug-in does not provide an editor.\n\n%1").arg(vst3()->lastError()));
      }

void Vst3Gui::unloadPlugin()
      {
      vst3()->unloadPlugin();
      updateControls();
      emit valueChanged();
      emit sfChanged();
      if (_routeToPiano->isChecked())
            emit requestPatchRouting(QStringLiteral("Fluid"));
      }

void Vst3Gui::synthesizerChanged()
      {
      if (vst3()->isLoaded()) {
            addPluginPath(vst3()->pluginPath(), true);
            saveCachedPlugins();
            }
      updateSelectionDetails();
      updateControls();
      }

void Vst3Gui::selectedPluginChanged(int)
      {
      updateSelectionDetails();
      saveCachedPlugins();
      }

void Vst3Gui::routeOptionChanged(bool enabled)
      {
      saveCachedPlugins();
      if (enabled && vst3()->isLoaded())
            emit requestPatchRouting(QStringLiteral("VST3"));
      }

void Vst3Gui::audioBufferChanged(int)
      {
      saveCachedPlugins();
      }

void Vst3Gui::servicePlugin()
      {
      vst3()->servicePlugin();
      }

void Vst3Gui::updateControls()
      {
      const bool loaded = vst3()->isLoaded();
      _editorButton->setEnabled(loaded);
      _unloadButton->setEnabled(loaded);
      _loadButton->setEnabled(!selectedPath().isEmpty());
      _status->setText(loaded
         ? tr("Loaded and ready: %1").arg(vst3()->pluginName())
         : tr("No instrument loaded"));
      _status->setStyleSheet(loaded
         ? QStringLiteral("color: #2e7d32; font-weight: bold;")
         : QStringLiteral("color: palette(mid);") );
      _message->setText(vst3()->lastError());
      }

} // namespace Ms
