//=============================================================================
//  MuseScore
//  VST3 plug-in instance
//=============================================================================

#include "vst3plugin.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QApplication>
#include <QDebug>

#include "audio/midi/event.h"
#include "vst3componenthandler.h"
#include "vst3editor.h"

#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "public.sdk/source/common/memorystream.h"

namespace Ms {

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace VST3::Hosting;

namespace {

bool resultOk(tresult result)
      {
      return result == kResultOk || result == kResultTrue;
      }

ParamValue normalized7Bit(int value)
      {
      return std::max(0.0, std::min(1.0, value / 127.0));
      }

} // namespace

Vst3Plugin::Vst3Plugin(const Vst3PluginDescriptor& descriptor)
   : _descriptor(descriptor), _events(512)
      {
      }

Vst3Plugin::~Vst3Plugin()
      {
      _initialized.store(false, std::memory_order_release);

      QMutexLocker locker(&_processMutex);
      if (_processor)
            _processor->setProcessing(false);
      if (_component)
            _component->setActive(false);
      if (_controller)
            _controller->setComponentHandler(nullptr);

      _processData.unprepare();
      _componentHandler = nullptr;
      _processor = nullptr;
      _controller = nullptr;
      _component = nullptr;
      _provider = nullptr;
      _module = nullptr;
      }

bool Vst3Plugin::initialize(float sampleRate, const Vst3PluginState* state)
      {
      if (initialized())
            return true;

      std::string error;
      _module = Module::create(_descriptor.path.toStdString(), error);
      if (!_module) {
            qWarning() << "Unable to load VST3 module" << _descriptor.path << QString::fromStdString(error);
            return false;
            }

      ClassInfo selectedClass;
      bool found = false;
      for (const ClassInfo& classInfo : _module->getFactory().classInfos()) {
            if (QString::fromStdString(classInfo.ID().toString()) == _descriptor.classId) {
                  selectedClass = classInfo;
                  found = true;
                  break;
                  }
            }
      if (!found) {
            qWarning() << "VST3 class is no longer present" << _descriptor.classId << _descriptor.path;
            return false;
            }

      _provider = owned(new PlugProvider(_module->getFactory(), selectedClass));
      if (!_provider || !_provider->initialize()) {
            qWarning() << "Unable to initialize VST3 provider" << _descriptor.displayName();
            return false;
            }

      _component = IPtr<IComponent>::adopt(_provider->getComponent());
      _controller = IPtr<IEditController>::adopt(_provider->getController());
      if (!_component || !_controller) {
            qWarning() << "VST3 plug-in has no component or edit controller" << _descriptor.displayName();
            return false;
            }

      _processor = FUnknownPtr<IAudioProcessor>(_component);
      if (!_processor) {
            qWarning() << "VST3 component is not an audio processor" << _descriptor.displayName();
            return false;
            }

      _componentHandler = new Vst3ComponentHandler(
         [this](ParamID id, ParamValue value) { addParameterChange(id, value); },
         [this]() { _dirty.store(true, std::memory_order_release); });
      _controller->setComponentHandler(_componentHandler);

      _sampleRate = sampleRate;
      if (state)
            applyState(*state);

      if (!activateBuses() || !setupProcessing())
            return false;

      buildMidiMapping();
      _initialized.store(true, std::memory_order_release);
      return true;
      }

bool Vst3Plugin::activateBuses()
      {
      bool eventBusFound = false;
      const int eventBusCount = _component->getBusCount(kEvent, kInput);
      for (int i = 0; i < eventBusCount; ++i) {
            BusInfo info {};
            if (!resultOk(_component->getBusInfo(kEvent, kInput, i, info)))
                  continue;
            if (!eventBusFound && (info.busType == kMain || (info.flags & BusInfo::kDefaultActive))) {
                  _eventInputBus = i;
                  eventBusFound = resultOk(_component->activateBus(kEvent, kInput, i, true));
                  }
            }
      if (!eventBusFound && eventBusCount > 0) {
            _eventInputBus = 0;
            eventBusFound = resultOk(_component->activateBus(kEvent, kInput, 0, true));
            }

      bool outputBusFound = false;
      const int outputBusCount = _component->getBusCount(kAudio, kOutput);
      for (int i = 0; i < outputBusCount; ++i) {
            BusInfo info {};
            if (!resultOk(_component->getBusInfo(kAudio, kOutput, i, info)))
                  continue;
            if (!outputBusFound && (info.busType == kMain || (info.flags & BusInfo::kDefaultActive))) {
                  _audioOutputBus = i;
                  outputBusFound = resultOk(_component->activateBus(kAudio, kOutput, i, true));
                  }
            }
      if (!outputBusFound && outputBusCount > 0) {
            _audioOutputBus = 0;
            outputBusFound = resultOk(_component->activateBus(kAudio, kOutput, 0, true));
            }

      if (!eventBusFound || !outputBusFound) {
            qWarning() << "VST3 instrument has no usable event input or audio output" << _descriptor.displayName();
            return false;
            }
      return true;
      }

bool Vst3Plugin::setupProcessing()
      {
      ProcessSetup setup {};
      setup.processMode = kRealtime;
      setup.symbolicSampleSize = kSample32;
      setup.maxSamplesPerBlock = MAX_BLOCK_SIZE;
      setup.sampleRate = _sampleRate;

      if (!resultOk(_processor->setupProcessing(setup))) {
            qWarning() << "VST3 setupProcessing failed" << _descriptor.displayName();
            return false;
            }

      _processData.prepare(*_component, MAX_BLOCK_SIZE, kSample32);
      _processData.inputEvents = &_events;
      _processData.inputParameterChanges = &_parameters;
      _processData.processContext = &_processContext;

      const tresult activeResult = _component->setActive(true);
      if (!resultOk(activeResult)) {
            qWarning() << "VST3 component setActive failed" << activeResult << _descriptor.displayName();
            return false;
            }
      const tresult processingResult = _processor->setProcessing(true);
      // Vienna Synchron Player/Pianos returns E_NOTIMPL here but processes
      // normally once the component is active. Do not reject an otherwise
      // valid instrument when this optional state notification is absent.
      if (!resultOk(processingResult) && processingResult != kNotImplemented) {
            qWarning() << "VST3 processor setProcessing failed" << processingResult << _descriptor.displayName();
            return false;
            }
      return true;
      }

void Vst3Plugin::buildMidiMapping()
      {
      FUnknownPtr<IMidiMapping> mapping(_controller);
      if (!mapping)
            return;

      for (int controller = 0; controller < kCountCtrlNumber; ++controller) {
            ParamID id = kNoParamId;
            if (resultOk(mapping->getMidiControllerAssignment(_eventInputBus, 0,
                                                               static_cast<CtrlNumber>(controller), id)))
                  _midiParameters[controller] = id;
            }
      }

void Vst3Plugin::addParameterChange(ParamID id, ParamValue value)
      {
      QMutexLocker locker(&_pendingParametersMutex);
      _pendingParameters.emplace_back(id, std::max(0.0, std::min(1.0, value)));
      }

void Vst3Plugin::queueController(int controller, double value)
      {
      const auto it = _midiParameters.find(controller);
      if (it != _midiParameters.end())
            addParameterChange(it->second, value);
      }

void Vst3Plugin::handleEvent(const PlayEvent& event)
      {
      if (!initialized())
            return;

      const int pitch = event.pitch();

      Steinberg::Vst::Event vstEvent {};
      vstEvent.busIndex = _eventInputBus;
      vstEvent.sampleOffset = 0;
      vstEvent.ppqPosition = 0.0;
      // Editing/audition events are delivered while the score transport is
      // stopped.  Mark events as live, as MuseScore 4's VST sequencer does,
      // so instruments can distinguish them from timeline playback events.
      vstEvent.flags = Steinberg::Vst::Event::kIsLive;

      switch (event.type()) {
            case ME_NOTEON:
                  if (event.velo() == 0) {
                        if (pitch < 0 || pitch >= static_cast<int>(_activeNotes.size())
                            || !_activeNotes[pitch])
                              break;
                        vstEvent.type = Steinberg::Vst::Event::kNoteOffEvent;
                        vstEvent.noteOff.channel = 0;
                        vstEvent.noteOff.pitch = static_cast<int16>(pitch);
                        vstEvent.noteOff.velocity = _activeNoteVelocities[pitch];
                        vstEvent.noteOff.noteId = -1;
                        vstEvent.noteOff.tuning = _activeNoteTunings[pitch];
                        _activeNotes[pitch] = false;
                        }
                  else {
                        if (pitch < 0 || pitch >= static_cast<int>(_activeNotes.size()))
                              break;
                        vstEvent.type = Steinberg::Vst::Event::kNoteOnEvent;
                        vstEvent.noteOn.channel = 0;
                        vstEvent.noteOn.pitch = static_cast<int16>(pitch);
                        vstEvent.noteOn.tuning = event.tuning();
                        vstEvent.noteOn.velocity = static_cast<float>(normalized7Bit(event.velo()));
                        vstEvent.noteOn.length = 0;
                        vstEvent.noteOn.noteId = -1;
                        _activeNotes[pitch] = true;
                        _activeNoteTunings[pitch] = vstEvent.noteOn.tuning;
                        _activeNoteVelocities[pitch] = vstEvent.noteOn.velocity;
                        _silenceOutputUntilNoteOn.store(false, std::memory_order_release);
                        }
                  _events.addEvent(vstEvent);
                  break;

            case ME_NOTEOFF:
                  if (pitch < 0 || pitch >= static_cast<int>(_activeNotes.size())
                      || !_activeNotes[pitch])
                        break;
                  vstEvent.type = Steinberg::Vst::Event::kNoteOffEvent;
                  vstEvent.noteOff.channel = 0;
                  vstEvent.noteOff.pitch = static_cast<int16>(pitch);
                  vstEvent.noteOff.velocity = _activeNoteVelocities[pitch];
                  vstEvent.noteOff.noteId = -1;
                  vstEvent.noteOff.tuning = _activeNoteTunings[pitch];
                  _activeNotes[pitch] = false;
                  _events.addEvent(vstEvent);
                  break;

            case ME_POLYAFTER:
                  vstEvent.type = Steinberg::Vst::Event::kPolyPressureEvent;
                  vstEvent.polyPressure.channel = 0;
                  vstEvent.polyPressure.pitch = static_cast<int16>(event.pitch());
                  vstEvent.polyPressure.pressure = static_cast<float>(normalized7Bit(event.value()));
                  vstEvent.polyPressure.noteId = -1;
                  _events.addEvent(vstEvent);
                  break;

            case ME_CONTROLLER:
                  if (event.controller() == CTRL_ALL_NOTES_OFF
                      || event.controller() == CTRL_ALL_SOUNDS_OFF)
                        allNotesOff();
                  else
                        queueController(event.controller(), normalized7Bit(event.value()));
                  break;

            case ME_AFTERTOUCH:
                  queueController(kAfterTouch, normalized7Bit(event.value()));
                  break;

            case ME_PITCHBEND: {
                  const int bend = (event.value() << 7) | event.dataA();
                  queueController(kPitchBend, std::max(0.0, std::min(1.0, bend / 16383.0)));
                  break;
                  }

            default:
                  break;
            }
      }

void Vst3Plugin::process(unsigned frames, float* interleavedStereo)
      {
      if (!initialized() || !interleavedStereo || frames == 0 || frames > MAX_BLOCK_SIZE)
            return;

      QMutexLocker locker(&_processMutex);

      std::vector<std::pair<ParamID, ParamValue>> pending;
      {
      QMutexLocker pendingLocker(&_pendingParametersMutex);
      pending.swap(_pendingParameters);
      }
      for (const auto& change : pending) {
            int32 queueIndex = 0;
            IParamValueQueue* queue = _parameters.addParameterData(change.first, queueIndex);
            if (queue) {
                  int32 pointIndex = 0;
                  queue->addPoint(0, change.second, pointIndex);
                  }
            }

      for (int bus = 0; bus < _processData.numOutputs; ++bus) {
            AudioBusBuffers& output = _processData.outputs[bus];
            for (int channel = 0; channel < output.numChannels; ++channel)
                  std::fill(output.channelBuffers32[channel], output.channelBuffers32[channel] + frames, 0.0f);
            }

      _processContext.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
      if (_playing.load(std::memory_order_acquire))
            _processContext.state |= ProcessContext::kPlaying;
      _processContext.sampleRate = _sampleRate;
      _processContext.projectTimeSamples = _samplePosition;
      _processContext.continousTimeSamples = _samplePosition;
      _processContext.tempo = _tempo.load(std::memory_order_acquire);
      _processContext.projectTimeMusic = (_samplePosition / _sampleRate) * (_processContext.tempo / 60.0);

      _processData.numSamples = static_cast<int32>(frames);
      const bool processed = resultOk(_processor->process(_processData));
      if (processed && !_silenceOutputUntilNoteOn.load(std::memory_order_acquire)
          && _processData.outputs && _audioOutputBus < _processData.numOutputs) {
            const AudioBusBuffers& output = _processData.outputs[_audioOutputBus];
            if (output.numChannels == 1) {
                  const float* mono = output.channelBuffers32[0];
                  for (unsigned i = 0; i < frames; ++i) {
                        interleavedStereo[i * 2] += mono[i];
                        interleavedStereo[i * 2 + 1] += mono[i];
                        }
                  }
            else if (output.numChannels >= 2) {
                  const float* left = output.channelBuffers32[0];
                  const float* right = output.channelBuffers32[1];
                  for (unsigned i = 0; i < frames; ++i) {
                        interleavedStereo[i * 2] += left[i];
                        interleavedStereo[i * 2 + 1] += right[i];
                        }
                  }
            }

      _events.clear();
      _parameters.clearQueue();
      _samplePosition += frames;
      }

void Vst3Plugin::allNotesOff()
      {
      for (int note = 0; note < static_cast<int>(_activeNotes.size()); ++note) {
            if (!_activeNotes[note])
                  continue;
            PlayEvent event(ME_NOTEOFF, 0, note, 0);
            handleEvent(event);
            }
      }

void Vst3Plugin::setPlaybackState(bool playing, double tempo)
      {
      const bool wasPlaying = _playing.exchange(playing, std::memory_order_acq_rel);
      if (tempo > 0.0)
            _tempo.store(tempo, std::memory_order_release);

      if (playing)
            _silenceOutputUntilNoteOn.store(false, std::memory_order_release);

      // Seq calls this once per audio block.  Clearing notes on every stopped
      // block cancels click/pitch-change audition notes before they can render.
      // Only flush when the transport actually transitions from play to stop.
      if (wasPlaying && !playing) {
            allNotesOff();
            // MuseScore 4 deactivates VST output when transport stops, so
            // controller resets cannot leak through a plug-in's release tail.
            // Keep processing for stopped note audition, but suppress output
            // until playback or a new audition note begins.
            _silenceOutputUntilNoteOn.store(true, std::memory_order_release);
            }
      }

Vst3PluginState Vst3Plugin::captureState()
      {
      Vst3PluginState result;
      if (!_component || !_controller)
            return result;

      QMutexLocker locker(&_processMutex);
      MemoryStream componentStream;
      if (resultOk(_component->getState(&componentStream)))
            result.component = QByteArray(componentStream.getData(), static_cast<int>(componentStream.getSize()));

      MemoryStream controllerStream;
      if (resultOk(_controller->getState(&controllerStream)))
            result.controller = QByteArray(controllerStream.getData(), static_cast<int>(controllerStream.getSize()));

      _dirty.store(false, std::memory_order_release);
      return result;
      }

bool Vst3Plugin::applyState(const Vst3PluginState& state)
      {
      if (!_component || !_controller)
            return false;

      bool ok = true;
      if (!state.component.isEmpty()) {
            MemoryStream stream(const_cast<char*>(state.component.constData()), state.component.size());
            ok = resultOk(_component->setState(&stream));
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            ok = resultOk(_controller->setComponentState(&stream)) && ok;
            }
      if (!state.controller.isEmpty()) {
            MemoryStream stream(const_cast<char*>(state.controller.constData()), state.controller.size());
            ok = resultOk(_controller->setState(&stream)) && ok;
            }
      return ok;
      }

IPtr<IPlugView> Vst3Plugin::createView()
      {
      if (!_controller)
            return nullptr;
      return IPtr<IPlugView>::adopt(_controller->createView(ViewType::kEditor));
      }

bool Vst3Plugin::hasEditor() const
      {
      return initialized() && _controller;
      }

bool Vst3Plugin::openEditor(QWidget* parent)
      {
      if (!hasEditor() || QApplication::closingDown())
            return false;

      if (_editor) {
            _editor->show();
            _editor->raise();
            _editor->activateWindow();
            return true;
            }

      auto* dialog = new Vst3EditorDialog(shared_from_this(), parent);
      if (!dialog->attachPluginView()) {
            delete dialog;
            return false;
            }
      _editor = dialog;
      dialog->show();
      dialog->raise();
      dialog->activateWindow();
      return true;
      }

} // namespace Ms
