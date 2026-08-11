//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2002-2012 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#ifndef __SYNTHESIZER_H__
#define __SYNTHESIZER_H__

#include "libmscore/synthesizerstate.h"

#include <atomic>

class QWidget;

namespace Ms {

struct MidiPatch;
class PlayEvent;
class Synth;
class SynthesizerGui;

//---------------------------------------------------------
//   SoundFontInfo
//---------------------------------------------------------

struct SoundFontInfo {
      QString fileName;
      QString fontName;

      SoundFontInfo(QString _fileName) : fileName(_fileName), fontName(_fileName) {}
      SoundFontInfo(QString _fileName, QString _fontName) : fileName(_fileName), fontName(_fontName) {}
      };

//---------------------------------------------------------
//   Synthesizer
//---------------------------------------------------------

class Synthesizer {
      std::atomic<bool> _active;

   protected:
      float _sampleRate { 44100.0f };
      SynthesizerGui* _gui { nullptr };

   public:
      Synthesizer() : _active(false) { _gui = 0; }
      virtual ~Synthesizer() {}
      virtual void init(float sr)    { _sampleRate = sr; }
      float sampleRate() const       { return _sampleRate; }

      virtual const char* name() const = 0;

      virtual void setMasterTuning(double) {}
      virtual double masterTuning() const { return 440.0; }

      virtual bool loadSoundFonts(const QStringList&) = 0;
      virtual bool addSoundFont(const QString&)    { return false; }
      virtual bool removeSoundFont(const QString&) { return false; }

      virtual std::vector<SoundFontInfo> soundFontsInfo() const = 0;

      virtual void process(unsigned, float*, float*, float*) = 0;
      virtual void play(const PlayEvent&) = 0;
      virtual void setPlaybackState(bool, double) {}

      // Hosts which need a main-thread construction step (notably VST3)
      // can prepare and release their per-score playback channels here.
      virtual bool prepareChannel(int, int, int) { return true; }
      virtual void releaseChannel(int) {}
      virtual bool hasEditor(int) const { return false; }
      virtual bool openEditor(int, QWidget*) { return false; }

      virtual const QList<MidiPatch*>& getPatchInfo() const = 0;

      // get/set synthesizer state
      // prepareState() may perform an explicit, potentially expensive snapshot.
      // state() itself must remain cheap because the playback renderer calls it.
      virtual void prepareState() {}
      virtual SynthesizerGroup state() const = 0;
      virtual bool setState(const SynthesizerGroup&) = 0;
      virtual void setValue(int, double) {}
      virtual double value(int) const { return 0.0; }

      virtual void reset()            { _active.store(false, std::memory_order_release); }
      bool active() const             { return _active.load(std::memory_order_acquire); }
      void setActive(bool val = true) { _active.store(val, std::memory_order_release); }

      virtual void allSoundsOff(int /*channel*/) {}
      virtual void allNotesOff(int /*channel*/) {}

      virtual SynthesizerGui* gui()  { return _gui; }
      };

}
#endif

