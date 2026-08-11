//=============================================================================
//  MuseScore
//  VST3 plug-in instance
//=============================================================================

#ifndef MS_VST3PLUGIN_H
#define MS_VST3PLUGIN_H

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QMutex>
#include <QPointer>

#include "vst3types.h"

#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

class QWidget;

namespace Ms {

class PlayEvent;
class Vst3ComponentHandler;
class Vst3EditorDialog;

class Vst3Plugin : public std::enable_shared_from_this<Vst3Plugin> {
      static constexpr int MAX_BLOCK_SIZE = 8192;

      Vst3PluginDescriptor _descriptor;
      VST3::Hosting::Module::Ptr _module;
      Steinberg::IPtr<Steinberg::Vst::PlugProvider> _provider;
      Steinberg::IPtr<Steinberg::Vst::IComponent> _component;
      Steinberg::IPtr<Steinberg::Vst::IEditController> _controller;
      Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> _processor;
      Steinberg::FUnknownPtr<Vst3ComponentHandler> _componentHandler;

      Steinberg::Vst::HostProcessData _processData;
      Steinberg::Vst::ProcessContext _processContext {};
      Steinberg::Vst::EventList _events;
      Steinberg::Vst::ParameterChanges _parameters;
      std::map<int, Steinberg::Vst::ParamID> _midiParameters;
      std::array<bool, 128> _activeNotes {};
      std::array<float, 128> _activeNoteTunings {};
      std::array<float, 128> _activeNoteVelocities {};

      QMutex _processMutex;
      QMutex _pendingParametersMutex;
      std::vector<std::pair<Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue>> _pendingParameters;

      std::atomic<bool> _initialized { false };
      std::atomic<bool> _playing { false };
      std::atomic<bool> _silenceOutputUntilNoteOn { false };
      std::atomic<bool> _dirty { false };
      std::atomic<double> _tempo { 120.0 };
      Steinberg::int64 _samplePosition { 0 };
      int _eventInputBus { 0 };
      int _audioOutputBus { 0 };
      float _sampleRate { 44100.0f };
      QPointer<Vst3EditorDialog> _editor;

      void addParameterChange(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue);
      void queueController(int, double);
      bool activateBuses();
      bool setupProcessing();
      void buildMidiMapping();

   public:
      explicit Vst3Plugin(const Vst3PluginDescriptor&);
      ~Vst3Plugin();

      bool initialize(float sampleRate, const Vst3PluginState* state = nullptr);
      bool initialized() const { return _initialized.load(std::memory_order_acquire); }
      const Vst3PluginDescriptor& descriptor() const { return _descriptor; }

      void handleEvent(const PlayEvent&);
      void process(unsigned frames, float* interleavedStereo);
      void allNotesOff();
      void setPlaybackState(bool playing, double tempo);

      Vst3PluginState captureState();
      bool applyState(const Vst3PluginState&);

      Steinberg::IPtr<Steinberg::IPlugView> createView();
      bool hasEditor() const;
      bool openEditor(QWidget* parent);
      };

} // namespace Ms

#endif // MS_VST3PLUGIN_H
