//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2010-2011 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "noteevent.h"
#include "xml.h"

namespace Ms {

//---------------------------------------------------------
//   read
//---------------------------------------------------------

void NoteEvent::read(XmlReader& e)
      {
      while (e.readNextStartElement()) {
            const QStringRef& tag(e.name());
            if (tag == "pitch")
                  _pitch = e.readInt();
            else if (tag == "ontime")
                  _ontime = e.readInt();
            else if (tag == "len")
                  _len = e.readInt();
            else if (tag == "velocity")
                  _velocity = e.readInt();
            else if (tag == "suppressTieTail")
                  _suppressTieTail = e.readBool();
            else if (tag == "playbackSourceIndex")
                  _playbackSourceIndex = e.readInt();
            else if (tag == "sourcePitch")
                  _sourcePitch = e.readInt();
            else if (tag == "sourceOntime")
                  _sourceOntime = e.readInt();
            else if (tag == "sourceLen")
                  _sourceLen = e.readInt();
            else if (tag == "sourceVelocity")
                  _sourceVelocity = e.readInt();
            else if (tag == "sourceSuppressTieTail")
                  _sourceSuppressTieTail = e.readBool();
            else if (tag == "suppressed")
                  _suppressed = e.readBool();
            else
                  e.unknown();
            }
      }

//---------------------------------------------------------
//   write
//---------------------------------------------------------

void NoteEvent::write(XmlWriter& xml) const
      {
      xml.stag("Event");
      xml.tag("pitch", _pitch, 0);
      xml.tag("ontime", _ontime, 0);
      xml.tag("len", _len, NOTE_LENGTH);
      xml.tag("velocity", _velocity, -1);
      xml.tag("suppressTieTail", _suppressTieTail, false);
      if (playbackTracked()) {
            xml.tag("playbackSourceIndex", _playbackSourceIndex);
            xml.tag("sourcePitch", _sourcePitch, 0);
            xml.tag("sourceOntime", _sourceOntime, 0);
            xml.tag("sourceLen", _sourceLen, NOTE_LENGTH);
            xml.tag("sourceVelocity", _sourceVelocity, -1);
            xml.tag("sourceSuppressTieTail", _sourceSuppressTieTail, false);
            xml.tag("suppressed", _suppressed, false);
            }
      xml.etag();
      }

void NoteEvent::trackPlaybackSource(int index)
      {
      if (playbackTracked())
            return;
      _playbackSourceIndex = index;
      _sourcePitch = _pitch;
      _sourceOntime = _ontime;
      _sourceLen = _len;
      _sourceVelocity = _velocity;
      _sourceSuppressTieTail = _suppressTieTail;
      }

void NoteEvent::trackPlaybackSource(int index, const NoteEvent& source)
      {
      _playbackSourceIndex = index;
      _sourcePitch = source._pitch;
      _sourceOntime = source._ontime;
      _sourceLen = source._len;
      _sourceVelocity = source._velocity;
      _sourceSuppressTieTail = source._suppressTieTail;
      }

void NoteEvent::detachPlaybackSource()
      {
      _playbackSourceIndex = -1;
      _sourcePitch = _pitch;
      _sourceOntime = _ontime;
      _sourceLen = _len;
      _sourceVelocity = _velocity;
      _sourceSuppressTieTail = _suppressTieTail;
      _suppressed = false;
      }

NoteEvent NoteEvent::rebasedOnto(const NoteEvent& generated) const
      {
      NoteEvent result = generated;
      result._pitch = generated._pitch + (_pitch - _sourcePitch);
      result._ontime = generated._ontime + (_ontime - _sourceOntime);
      result._len = qMax(1, generated._len + (_len - _sourceLen));
      result._velocity = (_velocity == _sourceVelocity) ? generated._velocity : _velocity;
      result._suppressTieTail = (_suppressTieTail == _sourceSuppressTieTail)
                              ? generated._suppressTieTail : _suppressTieTail;
      result._playbackSourceIndex = _playbackSourceIndex;
      result._sourcePitch = generated._pitch;
      result._sourceOntime = generated._ontime;
      result._sourceLen = generated._len;
      result._sourceVelocity = generated._velocity;
      result._sourceSuppressTieTail = generated._suppressTieTail;
      result._suppressed = _suppressed;
      return result;
      }

//---------------------------------------------------------
//   NoteEventList
//---------------------------------------------------------

NoteEventList::NoteEventList()
   : QList<NoteEvent>()
      {
      }

//---------------------------------------------------------
//   operator==
//---------------------------------------------------------

bool NoteEvent::operator==(const NoteEvent& e) const
      {
      return (e._pitch == _pitch) && (e._ontime == _ontime)
          && (e._len == _len) && (e._velocity == _velocity)
          && (e._suppressTieTail == _suppressTieTail)
          && (e._playbackSourceIndex == _playbackSourceIndex)
          && (e._sourcePitch == _sourcePitch)
          && (e._sourceOntime == _sourceOntime)
          && (e._sourceLen == _sourceLen)
          && (e._sourceVelocity == _sourceVelocity)
          && (e._sourceSuppressTieTail == _sourceSuppressTieTail)
          && (e._suppressed == _suppressed);
      }

}

