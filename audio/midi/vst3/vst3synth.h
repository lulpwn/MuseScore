//=============================================================================
//  MuseScore
//  VST3 instrument synthesizer
//
//  Copyright (C) 2026 MuseScore contributors
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.
//=============================================================================

#ifndef __VST3SYNTH_H__
#define __VST3SYNTH_H__

#include "audio/midi/synthesizer.h"

#include <memory>

class QWidget;

namespace Ms {

struct MidiPatch;
class PlayEvent;

//---------------------------------------------------------
//   Vst3Synth
//---------------------------------------------------------

class Vst3Synth final : public Synthesizer {
      class Impl;
      std::unique_ptr<Impl> _impl;
      QList<MidiPatch*> _patches;

      void updatePatchList();

   public:
      Vst3Synth();
      ~Vst3Synth() override;

      const char* name() const override { return "VST3"; }
      void init(float sampleRate) override;
      void process(unsigned frames, float* buffer, float*, float*) override;
      void play(const PlayEvent& event) override;
      void setPlaybackState(bool playing, double tempoBpm) override;
      void reset() override;

      bool loadPlugin(const QString& path, const QString& classUid = QString(),
                      const QByteArray& componentState = QByteArray(),
                      const QByteArray& controllerState = QByteArray());
      void unloadPlugin();
      bool isLoaded() const;
      QString pluginPath() const;
      QString pluginName() const;
      QString lastError() const;
      QStringList availablePlugins() const;
      bool isInstrumentPlugin(const QString& path) const;
      void servicePlugin();
      bool showEditor(QWidget* parent = nullptr);
      void closeEditor();

      bool loadSoundFonts(const QStringList& paths) override;
      bool addSoundFont(const QString& path) override;
      bool removeSoundFont(const QString& path) override;
      std::vector<SoundFontInfo> soundFontsInfo() const override;

      const QList<MidiPatch*>& getPatchInfo() const override { return _patches; }
      void prepareState() override;
      SynthesizerGroup state() const override;
      bool setState(const SynthesizerGroup& state) override;

      void allSoundsOff(int channel) override;
      void allNotesOff(int channel) override;
      SynthesizerGui* gui() override;
      };

} // namespace Ms

#endif
