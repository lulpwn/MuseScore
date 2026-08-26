//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2010-2012 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#ifndef __NOTEEVENT_H__
#define __NOTEEVENT_H__

#include <QtGlobal>

namespace Ms {

class XmlWriter;
class XmlReader;

//---------------------------------------------------------
//    NoteEvent
//---------------------------------------------------------

class NoteEvent {
      int _pitch;   // relative pitch to note pitch
      int _ontime;  // one unit is 1/1000 of nominal note len
      int _len;     // one unit is 1/1000 of nominal note len
      int _velocity; // absolute MIDI velocity override, -1 uses score dynamics
      bool _suppressTieTail; // event length already includes tied continuations
      // Piano-roll edits are stored as adjustments to an automatically
      // generated source event.  -2 means an untracked legacy/automatic
      // event, -1 is a piano-roll-only event, and >= 0 is the source index in
      // the freshly generated NoteEventList.
      int _playbackSourceIndex;
      int _sourcePitch;
      int _sourceOntime;
      int _sourceLen;
      int _sourceVelocity;
      bool _sourceSuppressTieTail;
      bool _suppressed;

   public:
      constexpr static int NOTE_LENGTH = 1000;

      NoteEvent() : _pitch(0), _ontime(0), _len(NOTE_LENGTH), _velocity(-1), _suppressTieTail(false),
         _playbackSourceIndex(-2), _sourcePitch(0), _sourceOntime(0), _sourceLen(NOTE_LENGTH),
         _sourceVelocity(-1), _sourceSuppressTieTail(false), _suppressed(false) {}
      NoteEvent(int a, int b, int c) : _pitch(a), _ontime(b), _len(c), _velocity(-1), _suppressTieTail(false),
         _playbackSourceIndex(-2), _sourcePitch(a), _sourceOntime(b), _sourceLen(c),
         _sourceVelocity(-1), _sourceSuppressTieTail(false), _suppressed(false) {}

      void read(XmlReader&);
      void write(XmlWriter&) const;

      int  pitch() const     { return _pitch; }
      int ontime() const     { return _ontime; }
      int offtime() const    { return _ontime + _len; }
      int len() const        { return _len; }
      int velocity() const   { return _velocity; }
      bool suppressTieTail() const { return _suppressTieTail; }
      bool playbackTracked() const { return _playbackSourceIndex != -2; }
      int playbackSourceIndex() const { return _playbackSourceIndex; }
      bool suppressed() const { return _suppressed; }
      void setPitch(int v)   { _pitch = v; }
      void setOntime(int v)  { _ontime = v; }
      void setLen(int v)     { _len = v;    }
      void setVelocity(int v) { _velocity = v < 0 ? -1 : qBound(1, v, 127); }
      void setSuppressTieTail(bool v) { _suppressTieTail = v; }
      void setSuppressed(bool v) { _suppressed = v; }
      void trackPlaybackSource(int index);
      void trackPlaybackSource(int index, const NoteEvent& source);
      void detachPlaybackSource();
      NoteEvent rebasedOnto(const NoteEvent&) const;
      bool operator==(const NoteEvent&) const;
      };

//---------------------------------------------------------
//   NoteEventList
//---------------------------------------------------------

class NoteEventList : public QList<NoteEvent> {
   public:
      NoteEventList();

      int offtime() { return empty() ? 0 : std::max_element(cbegin(), cend(), [](const NoteEvent& n1, const NoteEvent& n2) { return n1.offtime() < n2.offtime(); })->offtime(); }
      };


}     // namespace Ms
#endif
