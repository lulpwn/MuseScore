//=============================================================================
//  MuseScore
//  VST3 instrument synthesizer
//
//  Copyright (C) 2026 MuseScore contributors
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.
//=============================================================================

#include "vst3synth.h"
#include "vst3gui.h"

#include "audio/midi/event.h"
#include "audio/midi/midipatch.h"

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHideEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Ms {
namespace {

using Steinberg::FUnknown;
using Steinberg::FUnknownPrivate::iidEqual;
using Steinberg::IPtr;
using Steinberg::int32;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kNotImplemented;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::tresult;
using Steinberg::uint32;
using namespace Steinberg::Vst;

constexpr int kMaxBlockSize = 8192;
constexpr int kMidiChannels = 16;
constexpr int kMidiNotes = 128;

QByteArray streamData(Steinberg::ResizableMemoryIBStream& stream)
      {
      return QByteArray(static_cast<const char*>(stream.getData()), static_cast<int>(stream.getCursor()));
      }

void fillStream(Steinberg::ResizableMemoryIBStream& stream, const QByteArray& data)
      {
      if (!data.isEmpty()) {
            int32 written = 0;
            stream.write(const_cast<char*>(data.constData()), data.size(), &written);
            }
      stream.rewind();
      }

bool isInstrumentClass(const VST3::Hosting::ClassInfo& info)
      {
      if (info.category() != kVstAudioEffectClass)
            return false;
      for (const std::string& category : info.subCategories()) {
            if (category.find("Instrument") != std::string::npos)
                  return true;
            }
      return false;
      }

bool moduleContainsInstrument(const QString& path)
      {
      try {
            std::string error;
            const auto module = VST3::Hosting::Module::create(path.toUtf8().constData(), error);
            if (!module)
                  return false;
            const auto factory = module->getFactory();
            for (const auto& info : factory.classInfos()) {
                  if (isInstrumentClass(info))
                        return true;
                  }
            }
      catch (...) {
            }
      return false;
      }

//---------------------------------------------------------
//   PluginInstance
//---------------------------------------------------------

class PluginInstance final : public IComponentHandler {
      VST3::Hosting::Module::Ptr _module;
      IPtr<PlugProvider> _provider;
      IPtr<IComponent> _component;
      IPtr<IEditController> _controller;
      IPtr<IAudioProcessor> _processor;

      HostProcessData _processData;
      ProcessContext _processContext {};
      Steinberg::Vst::EventList _events;
      ParameterChanges _inputParameters;
      ParameterChanges _outputParameters;
      ParameterChangeTransfer _editorParameterChanges;
      ParameterChangeTransfer _outputParameterChanges;
      // The hosted instrument intentionally exposes a very small MIDI surface:
      // notes plus the damper pedal.  MuseScore emits bank/program, mixer,
      // expression, RPN, pitch-bend reset and other channel setup events during
      // normal transport operations.  Looking up only CC64 makes it impossible
      // for those events to become VST parameter changes accidentally.
      std::array<ParamID, kMidiChannels> _sustainAssignments;
      std::array<std::array<bool, kMidiNotes>, kMidiChannels> _activeNotes {};

      int32 _sampleSize { kSample32 };
      bool _processing { false };
      Steinberg::int64 _processedSamples { 0 };
      std::atomic<int32> _pendingRestartFlags { 0 };
      double _configuredSampleRate { 44100.0 };

      void initialiseMidiAssignments()
            {
            _sustainAssignments.fill(kNoParamId);

            const auto midiMapping = Steinberg::U::cast<IMidiMapping>(_controller);
            if (!midiMapping)
                  return;

            for (int channel = 0; channel < kMidiChannels; ++channel) {
                  ParamID parameter = kNoParamId;
                  if (midiMapping->getMidiControllerAssignment(
                         0, static_cast<Steinberg::int16>(channel),
                         kCtrlSustainOnOff, parameter) == kResultTrue)
                        _sustainAssignments[channel] = parameter;
                  }
            }

      void activateDefaultBuses()
            {
            const int32 eventInputs = _component->getBusCount(kEvent, kInput);
            for (int32 bus = 0; bus < eventInputs; ++bus)
                  _component->activateBus(kEvent, kInput, bus, bus == 0);

            const int32 audioInputs = _component->getBusCount(kAudio, kInput);
            for (int32 bus = 0; bus < audioInputs; ++bus)
                  _component->activateBus(kAudio, kInput, bus, false);

            const int32 audioOutputs = _component->getBusCount(kAudio, kOutput);
            // MuseScore currently consumes one output only.  Activating additional
            // buses based on their reported role is unsafe with older instruments
            // that incorrectly label multiple outputs as main buses.
            for (int32 bus = 0; bus < audioOutputs; ++bus)
                  _component->activateBus(kAudio, kOutput, bus, bus == 0);
            }

      bool restoreState(const QByteArray& componentState, const QByteArray& controllerState)
            {
            if (!componentState.isEmpty()) {
                  Steinberg::ResizableMemoryIBStream stream(componentState.size());
                  fillStream(stream, componentState);
                  if (_component->setState(&stream) != kResultOk)
                        return false;
                  if (_controller) {
                        stream.rewind();
                        _controller->setComponentState(&stream);
                        }
                  }

            if (_controller && !controllerState.isEmpty()) {
                  Steinberg::ResizableMemoryIBStream stream(controllerState.size());
                  fillStream(stream, controllerState);
                  _controller->setState(&stream);
                  }
            return true;
            }

      void addParameter(ParamID id, ParamValue value)
            {
            if (id == kNoParamId)
                  return;
            int32 queueIndex = 0;
            IParamValueQueue* queue = _inputParameters.addParameterData(id, queueIndex);
            if (queue) {
                  int32 pointIndex = 0;
                  queue->addPoint(0, std::clamp(value, 0.0, 1.0), pointIndex);
                  }
            }

      void addSustain(int channel, ParamValue value)
            {
            if (channel < 0 || channel >= kMidiChannels)
                  return;
            addParameter(_sustainAssignments[channel], value);
            }

      void addNoteOff(int channel, int pitch, float velocity = 0.0f)
            {
            if (channel < 0 || channel >= kMidiChannels || pitch < 0 || pitch >= kMidiNotes)
                  return;
            Steinberg::Vst::Event vstEvent {};
            vstEvent.busIndex = 0;
            vstEvent.sampleOffset = 0;
            vstEvent.ppqPosition = 0.0;
            vstEvent.flags = 0;
            vstEvent.type = Steinberg::Vst::Event::kNoteOffEvent;
            vstEvent.noteOff.channel = static_cast<Steinberg::int16>(channel);
            vstEvent.noteOff.pitch = static_cast<Steinberg::int16>(pitch);
            vstEvent.noteOff.velocity = velocity;
            vstEvent.noteOff.noteId = channel * kMidiNotes + pitch;
            _events.addEvent(vstEvent);
            _activeNotes[channel][pitch] = false;
            }

   public:
      QString path;
      QString displayName;
      QString classUid;

      PluginInstance()
         : _events(4096), _inputParameters(256), _outputParameters(256),
           _editorParameterChanges(2048), _outputParameterChanges(2048)
            {
            _sustainAssignments.fill(kNoParamId);
            }

      ~PluginInstance()
            {
            if (_controller)
                  _controller->setComponentHandler(nullptr);
            if (_processor && _processing)
                  _processor->setProcessing(false);
            if (_component)
                  _component->setActive(false);
            _processing = false;
            _processor = nullptr;
            _controller = nullptr;
            _component = nullptr;
            _provider = nullptr;
            _module.reset();
            }

      bool initialise(const QString& pluginPath, const QString& requestedClassUid,
                      const QByteArray& componentState, const QByteArray& controllerState,
                      double sampleRate, FUnknown* host, QString& error)
            {
            std::string sdkError;
            _module = VST3::Hosting::Module::create(pluginPath.toUtf8().constData(), sdkError);
            if (!_module) {
                  error = QString::fromStdString(sdkError);
                  if (error.isEmpty())
                        error = QObject::tr("The VST3 module could not be opened.");
                  return false;
                  }

            auto factory = _module->getFactory();
            factory.setHostContext(host);
            std::optional<VST3::Hosting::ClassInfo> selected;
            for (const auto& info : factory.classInfos()) {
                  if (!isInstrumentClass(info))
                        continue;
                  const QString uid = QString::fromStdString(info.ID().toString());
                  if (!requestedClassUid.isEmpty() && uid.compare(requestedClassUid, Qt::CaseInsensitive) != 0)
                        continue;
                  selected = info;
                  break;
                  }

            if (!selected) {
                  error = requestedClassUid.isEmpty()
                     ? QObject::tr("No VST3 instrument class was found in this module.")
                     : QObject::tr("The saved VST3 instrument class is no longer present in this module.");
                  return false;
                  }

            _provider = Steinberg::owned(new PlugProvider(factory, *selected, true));
            if (!_provider->initialize()) {
                  error = QObject::tr("The VST3 instrument failed during initialization.");
                  return false;
                  }

            _component = _provider->getComponentPtr();
            _controller = _provider->getControllerPtr();
            _processor = Steinberg::U::cast<IAudioProcessor>(_component);
            if (!_component || !_processor) {
                  error = QObject::tr("The module does not expose a VST3 audio processor.");
                  return false;
                  }

            if (_controller)
                  _controller->setComponentHandler(this);

            _component->setIoMode(kSimple);
            activateDefaultBuses();

            if (_component->getBusCount(kAudio, kOutput) < 1) {
                  error = QObject::tr("The VST3 instrument has no audio output bus.");
                  return false;
                  }

            if (_processor->canProcessSampleSize(kSample32) == kResultTrue)
                  _sampleSize = kSample32;
            else if (_processor->canProcessSampleSize(kSample64) == kResultTrue)
                  _sampleSize = kSample64;
            else {
                  error = QObject::tr("The VST3 instrument supports neither 32-bit nor 64-bit audio processing.");
                  return false;
                  }

            if (!restoreState(componentState, controllerState)) {
                  error = QObject::tr("The VST3 instrument rejected its saved state.");
                  return false;
                  }

            path = QDir::toNativeSeparators(QFileInfo(pluginPath).absoluteFilePath());
            displayName = QString::fromStdString(selected->name());
            classUid = QString::fromStdString(selected->ID().toString());
            initialiseMidiAssignments();

            if (!configureProcessing(sampleRate, error))
                  return false;
            return true;
            }

      bool configureProcessing(double sampleRate, QString& error)
            {
            if (!_processor || !_component)
                  return false;
            if (_processing)
                  _processor->setProcessing(false);
            _component->setActive(false);
            _processing = false;
            _processData.unprepare();
            activateDefaultBuses();

            ProcessSetup setup {};
            setup.processMode = kRealtime;
            setup.symbolicSampleSize = _sampleSize;
            setup.maxSamplesPerBlock = kMaxBlockSize;
            setup.sampleRate = sampleRate;
            if (_processor->setupProcessing(setup) != kResultOk) {
                  error = QObject::tr("The VST3 instrument rejected MuseScore's audio format.");
                  return false;
                  }

            _processData.prepare(*_component, kMaxBlockSize, _sampleSize);
            _processData.inputEvents = &_events;
            _processData.inputParameterChanges = &_inputParameters;
            _processData.outputEvents = nullptr;
            _processData.outputParameterChanges = &_outputParameters;
            _processData.processContext = &_processContext;

            _processContext = {};
            _processContext.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                                    ProcessContext::kContTimeValid;
            _processContext.sampleRate = sampleRate;
            _processContext.tempo = 120.0;
            _processContext.timeSigNumerator = 4;
            _processContext.timeSigDenominator = 4;

            if (_component->setActive(true) != kResultOk) {
                  error = QObject::tr("The VST3 instrument could not be activated.");
                  return false;
                  }
            // Some otherwise functional instruments (including Vienna Synchron
            // Player) return kResultFalse here.  Steinberg's own audiohost sample
            // deliberately treats that return value as advisory once setupProcessing
            // and setActive have succeeded.
            _processor->setProcessing(true);
            _processing = true;
            _configuredSampleRate = sampleRate;
            return true;
            // Processor-originated parameter changes belong on the controller/UI
            // thread.  Mirroring them here keeps animated plug-in controls in sync
            // without asking the audio callback to call into the editor.
            if (_controller) {
                  ParamID id = kNoParamId;
                  ParamValue value = 0.0;
                  int32 sampleOffset = 0;
                  while (_outputParameterChanges.getNextChange(id, value, sampleOffset))
                        _controller->setParamNormalized(id, value);
                  }
            }

      void serviceMainThread(QString& error)
            {
            const int32 restartFlags = _pendingRestartFlags.exchange(0);
            if (restartFlags & kMidiCCAssignmentChanged)
                  initialiseMidiAssignments();
            if (restartFlags & (kReloadComponent | kIoChanged | kLatencyChanged |
                                kPrefetchableSupportChanged)) {
                  if (!configureProcessing(_configuredSampleRate, error))
                        return;
                  }

            }

      bool needsProcessorRestart() const
            {
            const int32 flags = _pendingRestartFlags.load(std::memory_order_acquire);
            return flags & (kReloadComponent | kIoChanged | kLatencyChanged |
                            kPrefetchableSupportChanged);
            }

      bool process(unsigned frames, float* destination, bool playing, double tempoBpm)
            {

            if (!_processing || !_processor || frames == 0 || frames > kMaxBlockSize)
                  return false;

            _editorParameterChanges.transferChangesTo(_inputParameters);
            _processData.numSamples = static_cast<int32>(frames);
            _processContext.projectTimeSamples = _processedSamples;
            _processContext.continousTimeSamples = _processedSamples;
            _processContext.tempo = tempoBpm > 0.0 ? tempoBpm : 120.0;
            if (playing)
                  _processContext.state |= ProcessContext::kPlaying;
            else
                  _processContext.state &= ~ProcessContext::kPlaying;

            for (int32 bus = 0; bus < _processData.numInputs; ++bus) {
                  for (int32 channel = 0; channel < _processData.inputs[bus].numChannels; ++channel) {
                        if (_sampleSize == kSample32)
                              std::memset(_processData.inputs[bus].channelBuffers32[channel], 0, frames * sizeof(Sample32));
                        else
                              std::memset(_processData.inputs[bus].channelBuffers64[channel], 0, frames * sizeof(Sample64));
                        }
                  _processData.inputs[bus].silenceFlags = HostProcessData::kAllChannelsSilent;
                  }
            for (int32 bus = 0; bus < _processData.numOutputs; ++bus) {
                  for (int32 channel = 0; channel < _processData.outputs[bus].numChannels; ++channel) {
                        if (_sampleSize == kSample32)
                              std::memset(_processData.outputs[bus].channelBuffers32[channel], 0, frames * sizeof(Sample32));
                        else
                              std::memset(_processData.outputs[bus].channelBuffers64[channel], 0, frames * sizeof(Sample64));
                        }
                  _processData.outputs[bus].silenceFlags = 0;
                  }

            bool audible = false;
            if (_processor->process(_processData) == kResultOk) {
                  _outputParameterChanges.transferChangesFrom(_outputParameters);
                  if (_processData.numOutputs > 0) {
                  const AudioBusBuffers& output = _processData.outputs[0];
                  if (output.numChannels > 0) {
                        if (_sampleSize == kSample32) {
                              const Sample32* leftChannel = output.channelBuffers32[0];
                              const Sample32* rightChannel = output.numChannels > 1
                                                           ? output.channelBuffers32[1] : leftChannel;
                              for (unsigned frame = 0; frame < frames; ++frame) {
                                    const float left = leftChannel[frame];
                                    const float right = rightChannel[frame];
                                    if (std::isfinite(left)) {
                                          destination[frame * 2] += left;
                                          audible = audible || std::abs(left) > 1.0e-7f;
                                          }
                                    if (std::isfinite(right)) {
                                          destination[frame * 2 + 1] += right;
                                          audible = audible || std::abs(right) > 1.0e-7f;
                                          }
                                    }
                              }
                        else {
                              const Sample64* leftChannel = output.channelBuffers64[0];
                              const Sample64* rightChannel = output.numChannels > 1
                                                           ? output.channelBuffers64[1] : leftChannel;
                              for (unsigned frame = 0; frame < frames; ++frame) {
                                    const double left = leftChannel[frame];
                                    const double right = rightChannel[frame];
                                    if (std::isfinite(left)) {
                                          destination[frame * 2] += static_cast<float>(left);
                                          audible = audible || std::abs(left) > 1.0e-7;
                                          }
                                    if (std::isfinite(right)) {
                                          destination[frame * 2 + 1] += static_cast<float>(right);
                                          audible = audible || std::abs(right) > 1.0e-7;
                                          }
                                    }
                              }
                        }
                  }
                  }

            _processedSamples += frames;
            _events.clear();
            _inputParameters.clearQueue();
            _outputParameters.clearQueue();
            return audible;
            }

      void play(const PlayEvent& event)
            {
            const int channel = event.channel();
            if (channel < 0 || channel >= kMidiChannels)
                  return;

            if (event.type() == ME_NOTEON || event.type() == ME_NOTEOFF) {
                  const int pitch = event.dataA();
                  if (pitch < 0 || pitch >= kMidiNotes)
                        return;
                  const bool noteOn = event.type() == ME_NOTEON && event.dataB() > 0;
                  if (!noteOn) {
                        // Seq uses a 128-note sweep as a compatibility fallback
                        // for MIDI devices.  Do not pass that flood to a VST;
                        // release only notes this hosted instance actually owns.
                        if (_activeNotes[channel][pitch])
                              addNoteOff(channel, pitch, static_cast<float>(event.dataB()) / 127.0f);
                        return;
                        }

                  Steinberg::Vst::Event vstEvent {};
                  vstEvent.busIndex = 0;
                  vstEvent.sampleOffset = 0;
                  vstEvent.ppqPosition = 0.0;
                  vstEvent.flags = 0;
                  vstEvent.type = Steinberg::Vst::Event::kNoteOnEvent;
                  vstEvent.noteOn.channel = static_cast<Steinberg::int16>(channel);
                  vstEvent.noteOn.pitch = static_cast<Steinberg::int16>(pitch);
                  vstEvent.noteOn.velocity = static_cast<float>(event.dataB()) / 127.0f;
                  vstEvent.noteOn.tuning = event.tuning();
                  vstEvent.noteOn.length = 0;
                  vstEvent.noteOn.noteId = channel * kMidiNotes + pitch;
                  _events.addEvent(vstEvent);
                  _activeNotes[channel][pitch] = true;
                  return;
                  }

            if (event.type() == ME_CONTROLLER) {
                  const int controller = event.dataA();
                  if (controller == kCtrlAllNotesOff || controller == kCtrlAllSoundsOff) {
                        // Seq already emits explicit note-offs before these
                        // channel-mode messages.  Forwarding the message as a
                        // separate VST parameter queue makes its order relative
                        // to a same-block CC64 restoration undefined, and some
                        // instruments reset sustain after the restoration.
                        for (int pitch = 0; pitch < kMidiNotes; ++pitch) {
                              if (_activeNotes[channel][pitch])
                                    addNoteOff(channel, pitch);
                              }
                        return;
                        }
                  // CC64 is the only score controller required for the current
                  // piano playback host.  Everything else is deliberately
                  // discarded at the plug-in boundary.
                  if (controller == CTRL_SUSTAIN)
                        addSustain(channel, static_cast<double>(event.dataB()) / 127.0);
                  return;
                  }
            }

      void allNotesOff(int requestedChannel)
            {
            const int first = requestedChannel < 0 ? 0 : requestedChannel;
            const int last = requestedChannel < 0 ? kMidiChannels - 1 : requestedChannel;
            if (first < 0 || last >= kMidiChannels)
                  return;
            for (int channel = first; channel <= last; ++channel) {
                  for (int pitch = 0; pitch < kMidiNotes; ++pitch) {
                        if (_activeNotes[channel][pitch])
                              addNoteOff(channel, pitch);
                        }
                  addSustain(channel, 0.0);
                  }
            }

      QByteArray componentState() const
            {
            if (!_component)
                  return {};
            Steinberg::ResizableMemoryIBStream stream;
            return _component->getState(&stream) == kResultOk ? streamData(stream) : QByteArray();
            }

      QByteArray controllerState() const
            {
            if (!_controller)
                  return {};
            Steinberg::ResizableMemoryIBStream stream;
            return _controller->getState(&stream) == kResultOk ? streamData(stream) : QByteArray();
            }

      IPtr<IEditController> controller() const { return _controller; }

      tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
      tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override
            {
            _editorParameterChanges.addChange(id, valueNormalized, 0);
            return kResultOk;
            }
      tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
      tresult PLUGIN_API restartComponent(int32 flags) override
            {
            _pendingRestartFlags.fetch_or(flags);
            return kResultTrue;
            }

      tresult PLUGIN_API queryInterface(const Steinberg::TUID interfaceId, void** object) override
            {
            if (!object)
                  return kInvalidArgument;
            *object = nullptr;
            if (iidEqual(interfaceId, IComponentHandler::iid) || iidEqual(interfaceId, FUnknown::iid)) {
                  *object = static_cast<IComponentHandler*>(this);
                  addRef();
                  return kResultTrue;
                  }
            return kNoInterface;
            }
      uint32 PLUGIN_API addRef() override { return 1000; }
      uint32 PLUGIN_API release() override { return 1000; }
      };

//---------------------------------------------------------
//   Vst3EditorWindow
//---------------------------------------------------------

class Vst3EditorWindow final : public QWidget, public Steinberg::IPlugFrame {
      IPtr<Steinberg::IPlugView> _view;
      bool _attached { false };
      bool _resizingFromPlugin { false };
      bool _resizingToConstraint { false };
      bool _viewCanResize { false };

      void detach()
            {
            if (!_view || !_attached)
                  return;
            _view->setFrame(nullptr);
            _view->removed();
            _attached = false;
            }

   protected:
      bool event(QEvent* event) override
            {
            const bool handled = QWidget::event(event);
            if (_attached && _view) {
                  // The plug-in is a native child window.  The Qt container loses
                  // keyboard focus when that child receives it, even though the
                  // editor window is still active.  Forwarding that FocusOut as
                  // onFocus(false) can make some editors suspend repainting.
                  if (event->type() == QEvent::WindowActivate)
                        _view->onFocus(true);
                  else if (event->type() == QEvent::WindowDeactivate)
                        _view->onFocus(false);
                  }
            return handled;
            }

      void showEvent(QShowEvent* event) override
            {
            QWidget::showEvent(event);
            if (_attached || !_view)
                  return;
            _view->setFrame(this);
            if (_view->attached(reinterpret_cast<void*>(winId()), Steinberg::kPlatformTypeHWND) == kResultTrue) {
                  _attached = true;
                  _view->onFocus(isActiveWindow());
                  }
            else
                  _view->setFrame(nullptr);
            }

      void resizeEvent(QResizeEvent* event) override
            {
            QWidget::resizeEvent(event);
            if (!_attached || !_view || _resizingFromPlugin || _resizingToConstraint || !_viewCanResize)
                  return;

            Steinberg::ViewRect rect(0, 0, event->size().width(), event->size().height());
            _view->checkSizeConstraint(&rect);
            const QSize constrained(std::max(1, rect.right - rect.left),
                                    std::max(1, rect.bottom - rect.top));
            if (constrained != event->size()) {
                  _resizingToConstraint = true;
                  resize(constrained);
                  _resizingToConstraint = false;
                  }

            Steinberg::ViewRect current {};
            if (_view->getSize(&current) != kResultTrue ||
                current.right - current.left != constrained.width() ||
                current.bottom - current.top != constrained.height()) {
                  Steinberg::ViewRect constrainedRect(0, 0, constrained.width(), constrained.height());
                  _view->onSize(&constrainedRect);
                  }
            }

      void hideEvent(QHideEvent* event) override
            {
            if (_attached && _view)
                  _view->onFocus(false);
            QWidget::hideEvent(event);
            }

      void closeEvent(QCloseEvent* event) override
            {
            detach();
            QWidget::closeEvent(event);
            }

   public:
      Vst3EditorWindow(const IPtr<Steinberg::IPlugView>& view, const QString& title)
         : QWidget(nullptr, Qt::Window), _view(view)
            {
            setAttribute(Qt::WA_NativeWindow);
            setAttribute(Qt::WA_DeleteOnClose);
            setAttribute(Qt::WA_QuitOnClose, false);
            setWindowModality(Qt::NonModal);
            setFocusPolicy(Qt::NoFocus);
            setWindowTitle(title);
            Steinberg::ViewRect rect {};
            if (_view) {
                  _viewCanResize = _view->canResize() == kResultTrue;
                  if (auto scaleSupport = Steinberg::U::cast<Steinberg::IPlugViewContentScaleSupport>(_view))
                        scaleSupport->setContentScaleFactor(static_cast<float>(devicePixelRatioF()));
                  if (_view->getSize(&rect) == kResultTrue) {
                        const QSize initialSize(std::max(1, rect.right - rect.left),
                                                std::max(1, rect.bottom - rect.top));
                        resize(initialSize);
                        if (!_viewCanResize)
                              setFixedSize(initialSize);
                        }
                  }
            }

      ~Vst3EditorWindow() override { detach(); }

      tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override
            {
            if (!view || !newSize || view != _view.get())
                  return kInvalidArgument;
            if (_resizingFromPlugin)
                  return Steinberg::kResultFalse;

            Steinberg::ViewRect current {};
            if (_view->getSize(&current) == kResultTrue &&
                current.right - current.left == newSize->right - newSize->left &&
                current.bottom - current.top == newSize->bottom - newSize->top)
                  return kResultTrue;

            const QSize requested(std::max(1, newSize->right - newSize->left),
                                  std::max(1, newSize->bottom - newSize->top));
            _resizingFromPlugin = true;
            if (!_viewCanResize) {
                  setMinimumSize(0, 0);
                  setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
                  }
            resize(requested);
            if (!_viewCanResize)
                  setFixedSize(requested);
            _resizingFromPlugin = false;

            // Required by VST3: the host must call onSize in this same
            // resizeView call stack after resizing its platform window.
            _view->onSize(newSize);
            return kResultTrue;
            }

      tresult PLUGIN_API queryInterface(const Steinberg::TUID interfaceId, void** object) override
            {
            if (!object)
                  return kInvalidArgument;
            *object = nullptr;
            if (iidEqual(interfaceId, Steinberg::IPlugFrame::iid) || iidEqual(interfaceId, FUnknown::iid)) {
                  *object = static_cast<Steinberg::IPlugFrame*>(this);
                  addRef();
                  return kResultTrue;
                  }
            return kNoInterface;
            }
      uint32 PLUGIN_API addRef() override { return 1000; }
      uint32 PLUGIN_API release() override { return 1000; }
      };

} // namespace

//---------------------------------------------------------
//   Vst3Synth::Impl
//---------------------------------------------------------

class Vst3Synth::Impl {
   public:
      mutable std::mutex mutex;
      std::atomic<bool> servicing { false };
      std::atomic<bool> editorOpen { false };
      std::atomic<bool> playbackActive { false };
      std::atomic<double> playbackTempo { 120.0 };
      IPtr<HostApplication> host { Steinberg::owned(new HostApplication) };
      std::unique_ptr<PluginInstance> plugin;
      QPointer<Vst3EditorWindow> editor;
      QString error;
      QString componentState;
      QString controllerState;
      unsigned long long silentFrames { 0 };

      Impl()
            {
            PluginContextFactory::instance().setPluginContext(host.get());
            }

      ~Impl()
            {
            if (editor)
                  delete editor.data();
            plugin.reset();
            PluginContextFactory::instance().setPluginContext(nullptr);
            }
      };

//---------------------------------------------------------
//   createVst3Synth
//---------------------------------------------------------

} // namespace Ms

Ms::Synthesizer* createVst3Synth()
      {
      return new Ms::Vst3Synth();
      }

namespace Ms {

Vst3Synth::Vst3Synth()
   : Synthesizer(), _impl(new Impl)
      {
      }

Vst3Synth::~Vst3Synth()
      {
      closeEditor();
      qDeleteAll(_patches);
      _patches.clear();
      }

void Vst3Synth::init(float sampleRate)
      {
      Synthesizer::init(sampleRate);
      std::lock_guard<std::mutex> lock(_impl->mutex);
      if (_impl->plugin)
            _impl->plugin->configureProcessing(sampleRate, _impl->error);
      }

void Vst3Synth::process(unsigned frames, float* buffer, float*, float*)
      {
      // Playback and event delivery are serialized by Seq's audio callback.
      // Never skip a block on host-side contention: doing so creates an audible
      // gap, and skipping play() below loses the MIDI event permanently.
      if (_impl->servicing.load(std::memory_order_acquire))
            return;

      std::unique_lock<std::mutex> lock(_impl->mutex, std::try_to_lock);
      if (!lock.owns_lock()) {
            // A main-thread restart is deliberately allowed to silence one or
            // more blocks instead of making the PortAudio callback wait.
            if (_impl->servicing.load(std::memory_order_acquire))
                  return;
            lock.lock();
            }
      if (_impl->plugin) {
            const bool playing = _impl->playbackActive.load(std::memory_order_relaxed);
            const bool audible = _impl->plugin->process(
               frames, buffer, playing, _impl->playbackTempo.load(std::memory_order_relaxed));
            if (playing || _impl->editorOpen.load(std::memory_order_relaxed) || audible)
                  _impl->silentFrames = 0;
            else {
                  _impl->silentFrames += frames;
                  if (_impl->silentFrames >= static_cast<unsigned long long>(_sampleRate * 2.0f))
                        setActive(false);
                  }
            }
      }

void Vst3Synth::play(const PlayEvent& event)
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      if (_impl->plugin)
            _impl->plugin->play(event);
      }

void Vst3Synth::reset()
      {
      // A loaded instrument must keep receiving process calls even when no score
      // event has reached it yet.  Plug-in editor keyboards and audition controls
      // generate their sound internally during process().
      setActive(isLoaded() && _impl->editorOpen.load(std::memory_order_relaxed));
      }

bool Vst3Synth::loadPlugin(const QString& path, const QString& classUid,
                           const QByteArray& componentState, const QByteArray& controllerState)
      {
      auto replacement = std::make_unique<PluginInstance>();
      QString error;
      if (!replacement->initialise(path, classUid, componentState, controllerState,
                                   _sampleRate, _impl->host.get(), error)) {
            std::lock_guard<std::mutex> lock(_impl->mutex);
            _impl->error = error;
            return false;
            }

      // Take the initial snapshot before publishing the instance to the audio
      // thread.  Routine state() calls return this cache instead of serializing
      // a live plug-in during playback.
      const QByteArray replacementComponentState = replacement->componentState();
      const QByteArray replacementControllerState = replacement->controllerState();

      closeEditor();
      std::unique_ptr<PluginInstance> previous;
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      previous = std::move(_impl->plugin);
      _impl->plugin = std::move(replacement);
      _impl->componentState = QString::fromLatin1(replacementComponentState.toBase64());
      _impl->controllerState = QString::fromLatin1(replacementControllerState.toBase64());
      _impl->error.clear();
      _impl->silentFrames = 0;
      }
      // Plug-in destruction may be expensive; do it without holding the mutex
      // needed by the next real-time audio callback.
      previous.reset();
      setActive(true);
      updatePatchList();
      return true;
      }

void Vst3Synth::unloadPlugin()
      {
      closeEditor();
      std::unique_ptr<PluginInstance> previous;
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      previous = std::move(_impl->plugin);
      _impl->componentState.clear();
      _impl->controllerState.clear();
      _impl->error.clear();
      _impl->silentFrames = 0;
      }
      previous.reset();
      updatePatchList();
      reset();
      }

bool Vst3Synth::isLoaded() const
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      return bool(_impl->plugin);
      }

QString Vst3Synth::pluginPath() const
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      return _impl->plugin ? _impl->plugin->path : QString();
      }

QString Vst3Synth::pluginName() const
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      return _impl->plugin ? _impl->plugin->displayName : QString();
      }

QString Vst3Synth::lastError() const
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      return _impl->error;
      }

QStringList Vst3Synth::availablePlugins() const
      {
      QStringList result;
      try {
            const auto paths = VST3::Hosting::Module::getModulePaths();
            for (const std::string& path : paths) {
                  const QString pluginPath = QDir::toNativeSeparators(QString::fromUtf8(path.c_str()));
                  if (moduleContainsInstrument(pluginPath))
                        result.append(pluginPath);
                  }
            }
      catch (...) {
            }
      result.removeDuplicates();
      result.sort(Qt::CaseInsensitive);
      return result;
      }

bool Vst3Synth::isInstrumentPlugin(const QString& path) const
      {
      return !path.isEmpty() && moduleContainsInstrument(path);
      }

void Vst3Synth::servicePlugin()
      {
      // Only advertise a suspended processor when a plug-in actually requested
      // a heavyweight restart.  Routine 30 ms parameter/UI synchronisation must
      // never make the audio thread skip a block.
      const bool restarting = _impl->plugin && _impl->plugin->needsProcessorRestart();
      if (restarting)
            _impl->servicing.store(true, std::memory_order_release);
      std::unique_lock<std::mutex> lock(_impl->mutex, std::try_to_lock);
      if (lock.owns_lock() && _impl->plugin)
            _impl->plugin->serviceMainThread(_impl->error);
      if (restarting)
            _impl->servicing.store(false, std::memory_order_release);
      }

bool Vst3Synth::showEditor(QWidget* parent)
      {
      Q_UNUSED(parent);
      if (_impl->editor) {
            _impl->editor->show();
            _impl->editor->raise();
            _impl->editor->activateWindow();
            return true;
            }

      IPtr<IEditController> controller;
      QString title;
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      if (!_impl->plugin) {
            _impl->error = QObject::tr("No VST3 instrument is loaded.");
            return false;
            }
      controller = _impl->plugin->controller();
      title = _impl->plugin->displayName;
      }
      if (!controller) {
            _impl->error = QObject::tr("This VST3 instrument has no edit controller.");
            return false;
            }

      IPtr<Steinberg::IPlugView> view = Steinberg::owned(controller->createView(ViewType::kEditor));
      if (!view || view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue) {
            _impl->error = QObject::tr("This VST3 instrument does not provide a Windows editor.");
            return false;
            }

      _impl->editor = new Vst3EditorWindow(view, title);
      _impl->editorOpen.store(true, std::memory_order_release);
      QObject::connect(_impl->editor.data(), &QObject::destroyed, [this]() {
            _impl->editor = nullptr;
            _impl->editorOpen.store(false, std::memory_order_release);
            });
      setActive(true);
      _impl->editor->show();
      _impl->error.clear();
      return true;
      }

void Vst3Synth::closeEditor()
      {
      if (_impl->editor)
            delete _impl->editor.data();
      _impl->editor = nullptr;
      _impl->editorOpen.store(false, std::memory_order_release);
      }

bool Vst3Synth::loadSoundFonts(const QStringList& paths)
      {
      if (paths.isEmpty()) {
            unloadPlugin();
            return true;
            }
      return loadPlugin(paths.front());
      }

bool Vst3Synth::addSoundFont(const QString& path)
      {
      return loadPlugin(path);
      }

bool Vst3Synth::removeSoundFont(const QString& path)
      {
      if (QDir::cleanPath(path) != QDir::cleanPath(pluginPath()))
            return false;
      unloadPlugin();
      return true;
      }

std::vector<SoundFontInfo> Vst3Synth::soundFontsInfo() const
      {
      std::vector<SoundFontInfo> result;
      const QString path = pluginPath();
      if (!path.isEmpty())
            result.emplace_back(path, pluginName());
      return result;
      }

void Vst3Synth::prepareState()
      {
      // Only explicit Save to Score / Store actions request a live snapshot.
      // Background MIDI rendering calls state() frequently and must never make
      // the plug-in serialize its preset while the audio callback is running.
      std::lock_guard<std::mutex> lock(_impl->mutex);
      if (!_impl->plugin) {
            _impl->componentState.clear();
            _impl->controllerState.clear();
            return;
            }
      _impl->componentState = QString::fromLatin1(_impl->plugin->componentState().toBase64());
      _impl->controllerState = QString::fromLatin1(_impl->plugin->controllerState().toBase64());
      }

SynthesizerGroup Vst3Synth::state() const
      {
      SynthesizerGroup group;
      group.setName(name());
      std::lock_guard<std::mutex> lock(_impl->mutex);
      if (!_impl->plugin)
            return group;
      group.push_back(IdValue(0, _impl->plugin->path));
      group.push_back(IdValue(1, _impl->plugin->classUid));
      group.push_back(IdValue(2, _impl->componentState));
      group.push_back(IdValue(3, _impl->controllerState));
      return group;
      }

bool Vst3Synth::setState(const SynthesizerGroup& group)
      {
      QString path;
      QString uid;
      QByteArray componentState;
      QByteArray controllerState;
      for (const IdValue& value : group) {
            switch (value.id) {
                  case 0: path = value.data; break;
                  case 1: uid = value.data; break;
                  case 2: componentState = QByteArray::fromBase64(value.data.toLatin1()); break;
                  case 3: controllerState = QByteArray::fromBase64(value.data.toLatin1()); break;
                  default: break;
                  }
            }
      if (path.isEmpty()) {
            unloadPlugin();
            return true;
            }
      return loadPlugin(path, uid, componentState, controllerState);
      }

void Vst3Synth::allSoundsOff(int channel)
      {
      allNotesOff(channel);
      }

void Vst3Synth::allNotesOff(int channel)
      {
      std::lock_guard<std::mutex> lock(_impl->mutex);
      if (_impl->plugin)
            _impl->plugin->allNotesOff(channel);
      }

void Vst3Synth::updatePatchList()
      {
      qDeleteAll(_patches);
      _patches.clear();
      const QString loadedName = pluginName();
      if (!loadedName.isEmpty())
            _patches.append(new MidiPatch { false, name(), 0, 0, 10000,
                                           QObject::tr("VST3: %1").arg(loadedName) });
      }

SynthesizerGui* Vst3Synth::gui()
      {
      if (!_gui)
            _gui = new Vst3Gui(this);
      return _gui;
      }

void Vst3Synth::setPlaybackState(bool playing, double tempoBpm)
      {
      _impl->playbackActive.store(playing, std::memory_order_relaxed);
      if (tempoBpm > 0.0)
            _impl->playbackTempo.store(tempoBpm, std::memory_order_relaxed);
      }

} // namespace Ms
