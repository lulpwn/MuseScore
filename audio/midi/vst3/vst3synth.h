//=============================================================================
//  MuseScore
//  VST3 instrument synthesizer
//=============================================================================

#ifndef MS_VST3SYNTH_H
#define MS_VST3SYNTH_H

#include <array>
#include <memory>

#include <QMap>
#include <QMutex>
#include <QStringList>

#include "audio/midi/midipatch.h"
#include "audio/midi/synthesizer.h"
#include "vst3plugin.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"

namespace Ms {

class Vst3Gui;

class Vst3Synth : public Synthesizer {
      static constexpr int CHANNEL_COUNT = 256;

      struct SavedInstance {
            int bank { 0 };
            int program { 0 };
            QString path;
            QString classId;
            Vst3PluginState state;
            };

      std::array<std::shared_ptr<Vst3Plugin>, CHANNEL_COUNT> _instances;
      QList<MidiPatch*> _patches;
      QList<Vst3PluginDescriptor> _descriptors;
      QStringList _customDirectories;
      QStringList _scanErrors;
      QMap<int, SavedInstance> _savedInstances;
      SynthesizerGroup _stateCache;
      Steinberg::Vst::HostApplication _hostApplication;
      mutable QMutex _metadataMutex;

      const Vst3PluginDescriptor* descriptorForPatch(int bank, int program) const;
      void rebuildPatchList();
      void loadCustomDirectories();
      void saveCustomDirectories() const;
      QStringList pluginPaths() const;
      bool isInstrument(const VST3::Hosting::ClassInfo&) const;

   public:
      Vst3Synth();
      ~Vst3Synth() override;

      const char* name() const override { return "VST3"; }
      void init(float sampleRate) override;
      void process(unsigned, float*, float*, float*) override;
      void play(const PlayEvent&) override;
      void setPlaybackState(bool, double) override;

      bool prepareChannel(int channel, int bank, int program) override;
      void releaseChannel(int channel) override;
      bool hasEditor(int channel) const override;
      bool openEditor(int channel, QWidget* parent) override;
      void allSoundsOff(int channel) override;
      void allNotesOff(int channel) override;
      void reset() override;

      bool loadSoundFonts(const QStringList&) override;
      bool addSoundFont(const QString&) override;
      bool removeSoundFont(const QString&) override;
      std::vector<SoundFontInfo> soundFontsInfo() const override;
      const QList<MidiPatch*>& getPatchInfo() const override { return _patches; }

      void prepareState() override;
      SynthesizerGroup state() const override;
      bool setState(const SynthesizerGroup&) override;
      SynthesizerGui* gui() override;

      bool rescanPlugins();
      bool addCustomDirectory(const QString&);
      bool removeCustomDirectory(const QString&);
      QStringList customDirectories() const;
      QList<Vst3PluginDescriptor> descriptors() const;
      QStringList scanErrors() const;
      int activeChannel(int bank, int program) const;
      };

Synthesizer* createVst3Synth();

} // namespace Ms

#endif // MS_VST3SYNTH_H
