//=============================================================================
//  MuseScore
//  Key Editor model
//=============================================================================

#include "keyeditormodel.h"

#include "audio/midi/event.h"
#include "libmscore/beam.h"
#include "libmscore/chord.h"
#include "libmscore/measure.h"
#include "libmscore/note.h"
#include "libmscore/noteevent.h"
#include "libmscore/part.h"
#include "libmscore/pedal.h"
#include "libmscore/rest.h"
#include "libmscore/score.h"
#include "libmscore/segment.h"
#include "libmscore/spanner.h"
#include "libmscore/spannermap.h"
#include "libmscore/staff.h"
#include "libmscore/stem.h"
#include "libmscore/tie.h"
#include "libmscore/tuplet.h"
#include "libmscore/undo.h"
#include "libmscore/utils.h"

#include <QApplication>
#include <QClipboard>
#include <QDataStream>
#include <QMimeData>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Ms {

static const char* KEY_EDITOR_MIME = "application/x-musescore-key-editor-notes";
static const quint32 KEY_EDITOR_CLIPBOARD_MAGIC = 0x4b455933; // KEY3
static const quint16 KEY_EDITOR_CLIPBOARD_VERSION = 3;

static void undoEventListForLinkedNotes(Note* source, const NoteEventList& events)
      {
      if (!source)
            return;
      QSet<Note*> seen;
      for (ScoreElement* linkedElement : source->linkList()) {
            if (!linkedElement || linkedElement->type() != ElementType::NOTE)
                  continue;
            Note* linkedNote = toNote(linkedElement);
            if (!linkedNote->score() || seen.contains(linkedNote))
                  continue;
            seen.insert(linkedNote);
            NoteEventList linkedEvents = events;
            linkedNote->score()->undo(new ChangeNoteEventList(linkedNote, linkedEvents));
            }
      }

static void undoVelocityForLinkedNotes(Note* source, Note::ValueType type, int offset)
      {
      if (!source)
            return;
      QSet<Note*> seen;
      for (ScoreElement* linkedElement : source->linkList()) {
            if (!linkedElement || linkedElement->type() != ElementType::NOTE)
                  continue;
            Note* linkedNote = toNote(linkedElement);
            if (!linkedNote->score() || seen.contains(linkedNote))
                  continue;
            seen.insert(linkedNote);
            linkedNote->score()->undo(new ChangeVelocity(linkedNote, type, offset));
            }
      }

static qreal velocityRampProgress(ChangeMethod method, qreal progress)
      {
      progress = qBound<qreal>(0.0, progress, 1.0);
      constexpr qreal pi = 3.14159265358979323846;
      switch (method) {
            case ChangeMethod::EXPONENTIAL:
                  return progress <= 0.0 ? 0.0 : std::pow(2.0, progress) - 1.0;
            case ChangeMethod::EASE_IN:
                  return std::sin((progress - 1.0) * (pi / 2.0)) + 1.0;
            case ChangeMethod::EASE_OUT:
                  return std::sin(progress * (pi / 2.0));
            case ChangeMethod::EASE_IN_OUT:
                  return (std::sin(progress * pi - (pi / 2.0)) + 1.0) / 2.0;
            case ChangeMethod::NORMAL:
            default:
                  return progress;
            }
      }

KeyEditorModel::KeyEditorModel(QObject* parent)
   : QObject(parent)
      {
      }

void KeyEditorModel::clear()
      {
      _score = nullptr;
      _editStaff = nullptr;
      _visibleStaves.clear();
      _projectionRebuildPending = false;
      invalidateProjection();
      }

void KeyEditorModel::setProjectionUpdatesSuspended(bool suspended)
      {
      if (_projectionUpdatesSuspended == suspended)
            return;
      _projectionUpdatesSuspended = suspended;
      if (!suspended && _projectionRebuildPending) {
            _projectionRebuildPending = false;
            rebuild();
            }
      }

void KeyEditorModel::invalidateProjection()
      {
      _notes.clear();
      _selectedEvents.clear();
      _pedals.clear();
      _timeBuckets.clear();
      _noteLookup.clear();
      _scoreEndTick = 0;
      emit modelReset();
      }

void KeyEditorModel::invalidateElement(Element* element)
      {
      if (!element)
            return;

      bool changed = false;
      for (int i = _notes.size() - 1; i >= 0; --i) {
            if (static_cast<Element*>(_notes[i].note) == element) {
                  _notes.remove(i);
                  changed = true;
                  }
            }
      for (int i = _pedals.size() - 1; i >= 0; --i) {
            if (static_cast<Element*>(_pedals[i].spanner) == element) {
                  _pedals.remove(i);
                  changed = true;
                  }
            }

      if (!changed)
            return;

      // Element destruction can arrive midway through an undo command.  Drop
      // only the affected raw pointers here and retain the score extent, so a
      // temporary projection invalidation cannot clamp both scrollbars to 0.
      // The editor schedules a complete rebuild once the command has settled.
      rebuildIndexes();
      emit modelReset();
      }

bool KeyEditorModel::staffIsVisible(int staffIdx) const
      {
      for (Staff* staff : _visibleStaves) {
            if (staff && staff->idx() == staffIdx)
                  return true;
            }
      return false;
      }

void KeyEditorModel::setContext(Staff* editStaff, const QList<Staff*>& visibleStaves)
      {
      _editStaff = editStaff;
      _score = editStaff ? editStaff->score() : nullptr;
      _visibleStaves.clear();
      for (Staff* staff : visibleStaves) {
            if (staff && staff->score() == _score && !_visibleStaves.contains(staff))
                  _visibleStaves.append(staff);
            }
      if (_editStaff && !_visibleStaves.contains(_editStaff))
            _visibleStaves.prepend(_editStaff);
      rebuild();
      }

Note* KeyEditorModel::rootNote(Note* note) const
      {
      while (note && note->tieBack())
            note = note->tieBack()->startNote();
      return note;
      }

QString KeyEditorModel::eventKey(Note* note, int eventIndex) const
      {
      return QString::number(quintptr(note), 16) + QLatin1Char(':')
           + QString::number(eventIndex);
      }

Chord* KeyEditorModel::playbackAnchor(Note* note) const
      {
      if (!note || !note->chord())
            return nullptr;
      Chord* chord = note->chord();
      if (chord->isGrace() && chord->parent() && chord->parent()->isChord())
            return toChord(chord->parent());
      return chord;
      }

QSet<Note*> KeyEditorModel::tieChain(Note* original) const
      {
      QSet<Note*> result;
      Note* note = rootNote(original);
      while (note && !result.contains(note)) {
            result.insert(note);
            note = note->tieFor() ? note->tieFor()->endNote() : nullptr;
            }
      return result;
      }

int KeyEditorModel::playbackTieTail(Note* note, int eventIndex) const
      {
      if (!note || !note->tieFor() || eventIndex < 0
          || eventIndex != note->playEvents().size() - 1)
            return 0;

      int tail = 0;
      Note* continuation = note->tieFor()->endNote();
      QSet<Note*> seen;
      while (continuation && !seen.contains(continuation)) {
            seen.insert(continuation);
            bool startsGlissando = false;
            for (Spanner* spanner : continuation->spannerFor()) {
                  if (spanner && spanner->type() == ElementType::GLISSANDO) {
                        startsGlissando = true;
                        break;
                        }
                  }
            const NoteEventList& events = continuation->playEvents();
            if (events.size() != 1 || startsGlissando)
                  break;
            if (continuation->chord()) {
                  tail += continuation->chord()->actualTicks().ticks()
                        * events.front().len() / NoteEvent::NOTE_LENGTH;
                  }
            continuation = continuation->tieFor()
                         ? continuation->tieFor()->endNote() : nullptr;
            }
      return qMax(0, tail);
      }

bool KeyEditorModel::playbackBounds(Note* original, int& startTick, int& endTick) const
      {
      Note* note = rootNote(original);
      Chord* anchor = playbackAnchor(note);
      if (!note || !note->chord() || !anchor)
            return false;

      startTick = note->chord()->tick().ticks();
      endTick = startTick + qMax(1, note->playTicks());
      const bool grace = note->chord()->isGrace();
      if (note->playEvents().isEmpty())
            return true;

      const int anchorTicks = qMax(1, anchor->actualTicks().ticks());
      const int tieTail = grace ? 0 : qMax(0, note->playTicks() - note->chord()->actualTicks().ticks());
      int firstOn = INT_MAX;
      int lastOff = INT_MIN;
      for (const NoteEvent& event : note->playEvents()) {
            firstOn = qMin(firstOn, event.ontime());
            lastOff = qMax(lastOff, event.ontime() + qMax(1, event.len()));
            }
      if (firstOn == INT_MAX)
            return true;
      startTick = anchor->tick().ticks()
                + qRound(qreal(anchorTicks) * firstOn / NoteEvent::NOTE_LENGTH);
      endTick = anchor->tick().ticks()
              + qRound(qreal(anchorTicks) * lastOff / NoteEvent::NOTE_LENGTH) + tieTail;
      endTick = qMax(startTick + 1, endTick);
      if (startTick < 0) {
            endTick -= startTick;
            startTick = 0;
            }
      return true;
      }

int KeyEditorModel::effectiveVelocityInternal(Note* note) const
      {
      if (!note)
            return 1;
      Staff* staff = note->staff();
      if (!staff)
            return qBound(1, note->veloOffset(), 127);
      const Fraction tick = note->tick();
      int velocity = staff->velocities().val(tick);
      for (const VelocityOffsetRamp& ramp : staff->velocityOffsets()) {
            if (tick < ramp.start || tick > ramp.end)
                  continue;
            const Fraction length = ramp.end - ramp.start;
            const qreal progress = length.isZero() ? 1.0 : ((tick - ramp.start) / length).toDouble();
            velocity += qRound(ramp.change * velocityRampProgress(ramp.method, progress));
            }
      return qBound(1, note->customizeVelocity(qBound(1, velocity, 127)), 127);
      }

int KeyEditorModel::effectiveVelocity(Note* note) const
      {
      return effectiveVelocityInternal(rootNote(note));
      }

void KeyEditorModel::rebuildPedals()
      {
      _pedals.clear();
      if (!_score)
            return;
      for (const auto& entry : _score->spannerMap().map()) {
            Spanner* spanner = entry.second;
            if (!spanner || (!spanner->isPedal() && !spanner->isLetRing()))
                  continue;
            if (!staffIsVisible(spanner->staffIdx()))
                  continue;
            PedalBlock block;
            block.spanner = spanner;
            block.startTick = spanner->tick().ticks();
            block.endTick = qMax(block.startTick + 1, spanner->tick2().ticks());
            block.staffIdx = spanner->staffIdx();
            block.letRing = spanner->isLetRing();
            _pedals.append(block);
            }
      std::sort(_pedals.begin(), _pedals.end(), [](const PedalBlock& a, const PedalBlock& b) {
            if (a.startTick != b.startTick)
                  return a.startTick < b.startTick;
            return a.staffIdx < b.staffIdx;
            });
      }

void KeyEditorModel::rebuildIndexes()
      {
      _timeBuckets.clear();
      _noteLookup.clear();
      QHash<int, QVector<int> > pitchGroups;
      for (int index = 0; index < _notes.size(); ++index) {
            NoteBlock& block = _notes[index];
            block.overlap = false;
            if (!_noteLookup.contains(block.note))
                  _noteLookup.insert(block.note, index);
            const int firstBucket = block.startTick / _bucketTicks;
            const int lastBucket = qMax(block.startTick, block.endTick - 1) / _bucketTicks;
            for (int bucket = firstBucket; bucket <= lastBucket; ++bucket)
                  _timeBuckets[bucket].append(index);
            pitchGroups[block.pitch].append(index);
            }

      for (QVector<int>& group : pitchGroups) {
            std::sort(group.begin(), group.end(), [this](int a, int b) {
                  return _notes[a].startTick < _notes[b].startTick;
                  });
            QVector<int> active;
            for (int index : qAsConst(group)) {
                  const NoteBlock& current = _notes[index];
                  for (int i = active.size() - 1; i >= 0; --i) {
                        if (_notes[active[i]].endTick <= current.startTick)
                              active.remove(i);
                        }
                  for (int other : qAsConst(active)) {
                        _notes[other].overlap = true;
                        _notes[index].overlap = true;
                        }
                  active.append(index);
                  }
            }
      }

void KeyEditorModel::rebuild()
      {
      // renderMidi() regenerates NoteEventLists. Defer that mutation while the
      // sequencer's renderer may be reading the same lists.
      if (_projectionUpdatesSuspended) {
            _projectionRebuildPending = true;
            return;
            }
      if (_rebuilding)
            return;
      _rebuilding = true;
      _notes.clear();
      _scoreEndTick = 0;
      if (_score && _score->lastMeasure())
            _scoreEndTick = _score->lastMeasure()->endTick().ticks();

      if (_score && !_visibleStaves.isEmpty()) {
            // Use the same final MIDI event stream as playback. This includes
            // grace notes and every generated glissando, trill, tremolo and
            // arpeggio event without ornament-specific projection code.
            EventMap midiEvents;
            _score->renderMidi(&midiEvents, false, false, _score->synthesizerState());
            QHash<QString, QVector<int> > active;
            auto eventKey = [](const Note* note, int eventIndex, int pitch, int channel) {
                  return QString::number(quintptr(note), 16) + QLatin1Char(':')
                       + QString::number(eventIndex) + QLatin1Char(':')
                       + QString::number(pitch) + QLatin1Char(':')
                       + QString::number(channel);
                  };
            for (const auto& entry : midiEvents) {
                  const int tick = entry.first;
                  const NPlayEvent& event = entry.second;
                  if (event.type() != ME_NOTEON || !event.note())
                        continue;
                  Note* note = const_cast<Note*>(event.note());
                  const int staffIdx = event.getOriginatingStaff() >= 0
                                     ? event.getOriginatingStaff() : note->staffIdx();
                  if (!staffIsVisible(staffIdx))
                        continue;
                  const QString key = eventKey(note, event.noteEventIndex(),
                                               event.pitch(), event.channel());
                  if (event.velo() > 0) {
                        NoteBlock block;
                        block.note = note;
                        block.eventIndex = event.noteEventIndex();
                        block.startTick = tick;
                        block.endTick = tick + 1;
                        block.pitch = event.pitch();
                        block.staffIdx = staffIdx;
                        block.voice = note->voice();
                        block.velocity = event.velo();
                        block.grace = note->chord() && note->chord()->isGrace();
                        if (block.eventIndex >= 0 && block.eventIndex < note->playEvents().size()) {
                              const NoteEvent& sourceEvent = note->playEvents()[block.eventIndex];
                              block.ontime = sourceEvent.ontime();
                              block.eventLength = sourceEvent.len();
                              block.pitchOffset = sourceEvent.pitch();
                              }
                        active[key].append(_notes.size());
                        _notes.append(block);
                        }
                  else if (!active[key].isEmpty()) {
                        const int index = active[key].takeFirst();
                        _notes[index].endTick = qMax(_notes[index].startTick + 1, tick + 1);
                        _scoreEndTick = qMax(_scoreEndTick, _notes[index].endTick);
                        }
                  }
            }
      std::sort(_notes.begin(), _notes.end(), [](const NoteBlock& a, const NoteBlock& b) {
            if (a.startTick != b.startTick)
                  return a.startTick < b.startTick;
            if (a.pitch != b.pitch)
                  return a.pitch < b.pitch;
            if (a.staffIdx != b.staffIdx)
                  return a.staffIdx < b.staffIdx;
            return a.voice < b.voice;
            });
      QSet<QString> validEvents;
      for (const NoteBlock& block : qAsConst(_notes))
            validEvents.insert(eventKey(block.note, block.eventIndex));
      _selectedEvents.intersect(validEvents);
      if (_selectedEvents.isEmpty()) {
            for (const NoteBlock& block : qAsConst(_notes)) {
                  if (noteSelected(block.note))
                        _selectedEvents.insert(eventKey(block.note, block.eventIndex));
                  }
            }
      rebuildIndexes();
      rebuildPedals();
      _rebuilding = false;
      emit modelReset();
      }

void KeyEditorModel::syncSelection()
      {
      QSet<Note*> scoreSources;
      for (const NoteBlock& block : qAsConst(_notes)) {
            if (noteSelected(block.note))
                  scoreSources.insert(rootNote(block.note));
            }
      QSet<Note*> eventSources;
      for (const NoteBlock& block : qAsConst(_notes)) {
            if (_selectedEvents.contains(eventKey(block.note, block.eventIndex)))
                  eventSources.insert(rootNote(block.note));
            }
      // A piano-roll selection may contain only some generated events from a
      // source note. Preserve that subset when Score echoes the same source
      // selection back to its viewers; expand only a genuinely external one.
      if (scoreSources != eventSources) {
            _selectedEvents.clear();
            for (const NoteBlock& block : qAsConst(_notes)) {
                  if (scoreSources.contains(rootNote(block.note)))
                        _selectedEvents.insert(eventKey(block.note, block.eventIndex));
                  }
            }
      emit selectionChanged();
      }

QVector<int> KeyEditorModel::notesInRange(int startTick, int endTick, int lowPitch, int highPitch) const
      {
      if (endTick < startTick)
            std::swap(startTick, endTick);
      if (highPitch < lowPitch)
            std::swap(lowPitch, highPitch);
      startTick = qMax(0, startTick);
      lowPitch = qBound(0, lowPitch, 127);
      highPitch = qBound(0, highPitch, 127);

      QSet<int> candidates;
      const int firstBucket = startTick / _bucketTicks;
      const int lastBucket = qMax(startTick, endTick) / _bucketTicks;
      for (int bucket = firstBucket; bucket <= lastBucket; ++bucket) {
            const QVector<int> values = _timeBuckets.value(bucket);
            for (int index : values)
                  candidates.insert(index);
            }
      QVector<int> result;
      result.reserve(candidates.size());
      for (int index : qAsConst(candidates)) {
            const NoteBlock& block = _notes[index];
            if (block.endTick >= startTick && block.startTick <= endTick
                && block.pitch >= lowPitch && block.pitch <= highPitch)
                  result.append(index);
            }
      std::sort(result.begin(), result.end(), [this](int a, int b) {
            if (_notes[a].startTick != _notes[b].startTick)
                  return _notes[a].startTick < _notes[b].startTick;
            if (_notes[a].staffIdx != _notes[b].staffIdx)
                  return _notes[a].staffIdx < _notes[b].staffIdx;
            return _notes[a].voice < _notes[b].voice;
            });
      return result;
      }

int KeyEditorModel::noteAt(int tick, int pitch) const
      {
      const QVector<int> hits = notesInRange(tick, tick, pitch, pitch);
      for (auto it = hits.crbegin(); it != hits.crend(); ++it) {
            if (eventSelected(*it))
                  return *it;
            }
      return hits.isEmpty() ? -1 : hits.last();
      }

int KeyEditorModel::noteIndex(Note* note) const
      {
      return _noteLookup.value(rootNote(note), -1);
      }

int KeyEditorModel::noteIndex(Note* note, int eventIndex) const
      {
      for (int i = 0; i < _notes.size(); ++i) {
            if (_notes[i].note == note && _notes[i].eventIndex == eventIndex)
                  return i;
            }
      return -1;
      }

bool KeyEditorModel::noteSelected(Note* original) const
      {
      Note* note = rootNote(original);
      QSet<Note*> seen;
      while (note && !seen.contains(note)) {
            if (note->selected())
                  return true;
            seen.insert(note);
            note = note->tieFor() ? note->tieFor()->endNote() : nullptr;
            }
      return false;
      }

bool KeyEditorModel::eventSelected(int noteIndex) const
      {
      if (noteIndex < 0 || noteIndex >= _notes.size())
            return false;
      const NoteBlock& block = _notes[noteIndex];
      return _selectedEvents.contains(eventKey(block.note, block.eventIndex));
      }

bool KeyEditorModel::eventSelected(Note* note, int eventIndex) const
      {
      return note && _selectedEvents.contains(eventKey(note, eventIndex));
      }

QVector<int> KeyEditorModel::selectedEventIndexes() const
      {
      QVector<int> result;
      for (int i = 0; i < _notes.size(); ++i) {
            if (eventSelected(i))
                  result.append(i);
            }
      return result;
      }

QList<Note*> KeyEditorModel::selectedRootNotes() const
      {
      QList<Note*> result;
      QSet<Note*> seen;
      for (int i = 0; i < _notes.size(); ++i) {
            const NoteBlock& block = _notes[i];
            Note* note = rootNote(block.note);
            if (note && eventSelected(i) && !seen.contains(note)) {
                  seen.insert(note);
                  result.append(note);
                  }
            }
      return result;
      }

QList<Note*> KeyEditorModel::selectedNotes() const
      {
      return selectedRootNotes();
      }

void KeyEditorModel::select(const QList<Note*>& notes, SelectionOperation operation)
      {
      QSet<Note*> requestedNotes;
      for (Note* note : notes)
            requestedNotes.insert(rootNote(note));
      QVector<int> requested;
      for (int i = 0; i < _notes.size(); ++i) {
            if (requestedNotes.contains(rootNote(_notes[i].note)))
                  requested.append(i);
            }
      selectEvents(requested, operation);
      }

void KeyEditorModel::selectEvents(const QVector<int>& indexes, SelectionOperation operation)
      {
      if (!_score)
            return;
      QSet<QString> requested;
      for (int index : indexes) {
            if (index >= 0 && index < _notes.size()) {
                  const NoteBlock& block = _notes[index];
                  requested.insert(eventKey(block.note, block.eventIndex));
                  }
            }
      const QSet<QString> old = _selectedEvents;
      QSet<QString> next;
      switch (operation) {
            case SelectionOperation::Replace:
                  next = requested;
                  break;
            case SelectionOperation::Add:
                  next = old;
                  next.unite(requested);
                  break;
            case SelectionOperation::Toggle:
                  next = old;
                  for (const QString& key : qAsConst(requested)) {
                        if (next.contains(key))
                              next.remove(key);
                        else
                              next.insert(key);
                        }
                  break;
            case SelectionOperation::Subtract:
                  next = old;
                  next.subtract(requested);
                  break;
            }
      if (next == old)
            return;

      _selectedEvents = next;
      _score->startCmd();
      Selection& selection = _score->selection();
      selection.deselectAll();
      QSet<Note*> selectedSources;
      for (const NoteBlock& block : qAsConst(_notes)) {
            if (!_selectedEvents.contains(eventKey(block.note, block.eventIndex)))
                  continue;
            Note* note = rootNote(block.note);
            if (note && !selectedSources.contains(note)) {
                  selectedSources.insert(note);
                  selection.add(note);
                  _score->addRefresh(note->canvasBoundingRect());
                  }
            }
      _score->endCmd();
      emit selectionChanged();
      }

void KeyEditorModel::selectRange(int startTick, int endTick, int lowPitch, int highPitch,
                                 SelectionOperation operation)
      {
      selectEvents(notesInRange(startTick, endTick, lowPitch, highPitch), operation);
      }

void KeyEditorModel::selectAllVisible()
      {
      QVector<int> indexes;
      indexes.reserve(_notes.size());
      for (int i = 0; i < _notes.size(); ++i)
            indexes.append(i);
      selectEvents(indexes, SelectionOperation::Replace);
      }

void KeyEditorModel::clearSelection()
      {
      selectEvents(QVector<int>(), SelectionOperation::Replace);
      }

KeyEditorModel::NoteSnapshot KeyEditorModel::snapshot(Note* original) const
      {
      NoteSnapshot value;
      Note* note = rootNote(original);
      if (!note || !note->chord())
            return value;
      value.source = note;
      if (note->chord()->isGrace()) {
            int displayEnd = 1;
            if (!playbackBounds(note, value.startTick, displayEnd))
                  return NoteSnapshot();
            value.durationTicks = qMax(1, displayEnd - value.startTick);
            }
      else {
            value.startTick = note->chord()->tick().ticks();
            value.durationTicks = qMax(1, note->playTicks());
            }
      value.pitch = note->pitch();
      value.staffIdx = note->staffIdx();
      value.voice = note->voice();
      value.velocity = note->veloOffset();
      value.velocityType = int(note->veloType());
      value.tuning = note->tuning();
      value.subchannel = note->subchannel();
      value.play = note->play();
      QSet<Note*> fragmentSeen;
      for (Note* fragment = note; fragment && !fragmentSeen.contains(fragment);
           fragment = fragment->tieFor() ? fragment->tieFor()->endNote() : nullptr) {
            fragmentSeen.insert(fragment);
            value.sourceFragments.append(fragment);
            value.sourceTies.append(fragment->tieFor());
            value.fragmentOffsets.append(fragment->tick().ticks() - value.startTick);
            value.fragmentDurations.append(fragment->chord()->actualTicks().ticks());
            value.fragmentUserEvents.append(
                  fragment->chord()->playEventType() == PlayEventType::User);
            }
      value.userEvents = !value.fragmentUserEvents.isEmpty()
                       && value.fragmentUserEvents.front();
      if (value.userEvents) {
            for (const NoteEvent& event : note->playEvents()) {
                  EventSnapshot eventValue;
                  eventValue.ontime = event.ontime();
                  eventValue.length = event.len();
                  eventValue.pitch = event.pitch();
                  value.events.append(eventValue);
                  }
            }
      return value;
      }

bool KeyEditorModel::usesUserEventsAt(const NoteSnapshot& value, int relativeTick) const
      {
      if (relativeTick < 0)
            return false;
      const int count = qMin(value.fragmentOffsets.size(), value.fragmentUserEvents.size());
      for (int i = 0; i < count; ++i) {
            if (value.fragmentOffsets[i] == relativeTick)
                  return value.fragmentUserEvents[i];
            }
      // Clipboard snapshots intentionally contain only the logical root's
      // custom event list.  Tied continuations should remain automatic.
      return relativeTick == 0 && value.userEvents;
      }

QVector<KeyEditorModel::NoteSnapshot> KeyEditorModel::selectedSnapshots() const
      {
      QVector<NoteSnapshot> result;
      for (Note* note : selectedRootNotes())
            result.append(snapshot(note));
      return result;
      }

QVector<Note*> KeyEditorModel::segmentNotes(Segment* segment, int track) const
      {
      QVector<Note*> result;
      if (!segment)
            return result;
      ChordRest* chordRest = segment->cr(track);
      if (!chordRest || !chordRest->isChord())
            return result;
      for (Note* note : toChord(chordRest)->notes())
            result.append(note);
      return result;
      }

bool KeyEditorModel::sourceCanBeRewritten(Note* original, const QSet<Note*>& removal) const
      {
      Note* note = rootNote(original);
      QSet<Note*> seen;
      while (note && !seen.contains(note)) {
            seen.insert(note);
            Chord* chord = note->chord();
            if (!chord || chord->isGrace() || chord->tuplet())
                  return false;

            bool removesWholeChord = true;
            for (Note* chordNote : chord->notes()) {
                  if (!removal.contains(chordNote)) {
                        removesWholeChord = false;
                        break;
                        }
                  }
            if (removesWholeChord) {
                  const bool customBeam = chord->beamMode() != Beam::Mode::AUTO
                                       || (chord->beam() && !chord->beam()->generated());
                  const bool customStem = chord->stemDirection() != Direction::AUTO
                                       || chord->noStem() || chord->stemSlash()
                                       || (chord->stem() && !qFuzzyIsNull(chord->stem()->userLen()));
                  const bool customChordLayout = chord->isSmall() || chord->staffMove() != 0
                                              || !chord->offset().isNull() || !chord->visible()
                                              || (chord->crossMeasure() != CrossMeasure::UNKNOWN
                                                  && chord->crossMeasure() != CrossMeasure::NONE);
                  if (!chord->articulations().isEmpty() || chord->arpeggio()
                      || chord->tremolo() || !chord->graceNotes().isEmpty()
                      || !chord->lyrics().empty() || !chord->el().empty()
                      || customBeam || customStem || customChordLayout)
                        return false;

                  // Slurs and other chord-anchored spanners are not note
                  // children, so deleting the final note would otherwise
                  // leave their endpoints behind or silently discard them.
                  const int chordTick = chord->tick().ticks();
                  for (const auto& interval : chord->score()->spannerMap().findOverlapping(
                             chordTick, chordTick)) {
                        Spanner* spanner = interval.value;
                        if (spanner && (spanner->startElement() == chord
                                        || spanner->endElement() == chord))
                              return false;
                        }
                  }

            // Note-anchored glissandi and similar spanners cannot be cloned by a
            // rhythmic rewrite without explicitly retargeting both endpoints.
            if (!note->spannerFor().isEmpty() || !note->spannerBack().isEmpty())
                  return false;

            // Tie's copy constructor retains semantic properties such as
            // direction and line type, but it deliberately does not copy
            // laid-out segments.  Reject hand-shaped ties instead of losing
            // their grip offsets during a rhythmic rewrite or duplicate.
            if (Tie* tie = note->tieFor()) {
                  for (int i = 0; i < int(tie->spannerSegments().size()); ++i) {
                        if (tie->segmentAt(i)->isEdited())
                              return false;
                        }
                  }

            note = note->tieFor() ? note->tieFor()->endNote() : nullptr;
            }
      return true;
      }

bool KeyEditorModel::canPlace(const NoteSnapshot& value, const QSet<Note*>& removal) const
      {
      if (!_score || !_editStaff || value.durationTicks <= 0)
            return false;
      const int startTick = qMax(0, value.startTick);
      const int endTick = startTick + value.durationTicks;
      if (endTick > _scoreEndTick || startTick >= _scoreEndTick)
            return false;
      Staff* staff = _score->staff(value.staffIdx);
      if (!staff || staff->part() != _editStaff->part())
            return false;
      const int track = staff->idx() * VOICES + qBound(0, value.voice, VOICES - 1);

      int position = startTick;
      int guard = 0;
      while (position < endTick && guard++ < 100000) {
            ChordRest* current = _score->findCR(Fraction::fromTicks(position), track);
            if (!current) {
                  // Empty secondary voices are expanded by addNote().  Failure
                  // remains atomic because the caller rolls the command back.
                  return (track % VOICES) != 0
                         && _score->tick2segment(Fraction::fromTicks(position));
                  }
            if (current->tuplet())
                  return false;
            const int currentStart = current->tick().ticks();
            const int currentEnd = (current->tick() + current->actualTicks()).ticks();
            if (position < currentStart || position >= currentEnd)
                  return false;

            bool becomesRest = false;
            if (current->isChord()) {
                  Chord* chord = toChord(current);
                  becomesRest = true;
                  for (Note* chordNote : chord->notes()) {
                        if (!removal.contains(chordNote)) {
                              becomesRest = false;
                              if (chordNote->pitch() == value.pitch)
                                    return false; // same-pitch notes cannot coexist in one voice
                              }
                        }
                  }

            if (current->isChord() && !becomesRest) {
                  if ((toChord(current)->playEventType() == PlayEventType::User)
                      != usesUserEventsAt(value, position - startTick))
                        return false;
                  if (position != currentStart)
                        return false; // would split unrelated notes on the left
                  if (endTick < currentEnd)
                        return false; // would split unrelated notes on the right
                  }

            const int next = qMin(endTick, currentEnd);
            if (next <= position)
                  return false;
            position = next;
            }
      return position == endTick;
      }

void KeyEditorModel::addTie(Note* note)
      {
      if (!note || note->tieFor())
            return;
      Note* endNote = searchTieNote(note);
      if (!endNote)
            return;
      Tie* tie = new Tie(_score);
      tie->setStartNote(note);
      tie->setEndNote(endNote);
      tie->setTrack(note->track());
      tie->setTick(note->tick());
      tie->setTicks(endNote->tick() - note->tick());
      _score->undoAddElement(tie);
      }

bool KeyEditorModel::cutChordRest(ChordRest* target, int track, int cutTick,
                                  ChordRest*& first, ChordRest*& second)
      {
      first = target;
      second = nullptr;
      if (!target)
            return false;
      const Fraction start = target->tick();
      Fraction measureToTuplet(1, 1);
      Fraction tupletToMeasure(1, 1);
      if (target->tuplet()) {
            measureToTuplet = target->tuplet()->ratio();
            tupletToMeasure = measureToTuplet.inverse();
            }
      const Fraction measureDuration = target->ticks() * tupletToMeasure;
      const Fraction cut = Fraction::fromTicks(cutTick);
      if (cut <= start || cut >= start + measureDuration)
            return false;

      if (target->isChord()) {
            for (Note* note : toChord(target)->notes())
                  note->setSelected(false);
            }
      else if (target->isRest())
            target->setSelected(false);

      _score->setNoteRest(target->segment(), track, NoteVal(-1), (cut - start) * measureToTuplet);
      ChordRest* next = _score->findCR(cut, track);
      if (!next)
            return false;

      Chord* firstChord = nullptr;
      if (next->isChord()) {
            Chord* secondChord = toChord(next);
            for (Note* source : secondChord->notes()) {
                  if (!firstChord) {
                        ChordRest* firstRest = _score->findCR(start, track);
                        Segment* segment = _score->setNoteRest(firstRest->segment(), track,
                                                              source->noteVal(), firstRest->ticks());
                        firstChord = segment ? toChord(segment->cr(track)) : nullptr;
                        if (firstChord)
                              addTie(firstChord->notes().front());
                        }
                  else {
                        Note* added = _score->addNote(firstChord, source->noteVal());
                        addTie(added);
                        }
                  }
            first = firstChord;
            }
      else
            first = _score->findCR(start, track);
      second = next;
      return true;
      }

QVector<Note*> KeyEditorModel::addNote(const NoteSnapshot& snapshotValue)
      {
      QVector<Note*> added;
      if (!_score || snapshotValue.durationTicks <= 0)
            return added;
      Staff* staff = _score->staff(snapshotValue.staffIdx);
      if (!staff || !_editStaff || staff->part() != _editStaff->part())
            staff = _editStaff;
      if (!staff)
            return added;
      const int track = staff->idx() * VOICES + qBound(0, snapshotValue.voice, VOICES - 1);
      Fraction start = Fraction::fromTicks(qMax(0, snapshotValue.startTick));
      Fraction remaining = Fraction::fromTicks(snapshotValue.durationTicks);
      ChordRest* current = _score->findCR(start, track);
      if (!current) {
            Segment* segment = _score->tick2segment(start);
            if (segment)
                  _score->expandVoice(segment, track);
            current = _score->findCR(start, track);
            }
      if (!current)
            return added;

      if (start > current->tick()) {
            ChordRest* before = nullptr;
            ChordRest* after = nullptr;
            if (!cutChordRest(current, track, start.ticks(), before, after))
                  return added;
            current = after ? after : _score->findCR(start, track);
            }

      Fraction position = start;
      while (current && remaining > Fraction(0, 1)) {
            if (current->tuplet())
                  break;
            const Fraction available = current->actualTicks();
            const Fraction piece = remaining < available ? remaining : available;
            QVector<Note*> pieceNotes;
            if (current->isChord()) {
                  // canPlace() guarantees that an existing chord is consumed
                  // whole, so unrelated pitches never need a lossy split.
                  if (piece != available)
                        break;
                  Chord* chord = toChord(current);
                  auto found = std::find_if(chord->notes().begin(), chord->notes().end(),
                                             [&snapshotValue](Note* note) {
                                                   return note->pitch() == snapshotValue.pitch;
                                                   });
                  if (found != chord->notes().end())
                        break;
                  if (Note* created = _score->addNote(chord, NoteVal(snapshotValue.pitch)))
                        pieceNotes.append(created);
                  }
            else {
                  _score->setNoteRest(current->segment(), track, NoteVal(snapshotValue.pitch), piece);
                  const Fraction pieceEnd = position + piece;
                  for (Segment* segment = _score->tick2segment(position, false, SegmentType::ChordRest);
                       segment && segment->tick() < pieceEnd;
                       segment = segment->next1(SegmentType::ChordRest)) {
                        if (segment->tick() < position)
                              continue;
                        ChordRest* chordRest = segment->cr(track);
                        if (!chordRest || !chordRest->isChord())
                              continue;
                        Chord* chord = toChord(chordRest);
                        auto found = std::find_if(chord->notes().begin(), chord->notes().end(),
                                                  [&snapshotValue](Note* note) {
                                                        return note->pitch() == snapshotValue.pitch;
                                                        });
                        if (found != chord->notes().end())
                              pieceNotes.append(*found);
                        }
                  }
            if (pieceNotes.isEmpty())
                  break;

            std::sort(pieceNotes.begin(), pieceNotes.end(), [](Note* a, Note* b) {
                  return a->tick() < b->tick();
                  });

            if (!snapshotValue.source) {
                  for (Note* note : qAsConst(pieceNotes)) {
                        for (ScoreElement* linkedElement : note->linkList()) {
                              if (!linkedElement || linkedElement->type() != ElementType::NOTE)
                                    continue;
                              Note* linkedNote = toNote(linkedElement);
                              linkedNote->setVeloType(Note::ValueType(snapshotValue.velocityType));
                              linkedNote->setVeloOffset(snapshotValue.velocity);
                              linkedNote->setTuning(snapshotValue.tuning);
                              linkedNote->setSubchannel(snapshotValue.subchannel);
                              linkedNote->setPlay(snapshotValue.play);
                              }
                        }
                  }
            for (Note* note : qAsConst(pieceNotes)) {
                  if (!added.contains(note))
                        added.append(note);
                  }

            position += piece;
            remaining -= piece;
            if (remaining <= Fraction(0, 1))
                  break;
            current = _score->findCR(position, track);
            }

      for (int i = 0; i + 1 < added.size(); ++i)
            addTie(added[i]);

      if (snapshotValue.source) {
            if (snapshotValue.sourceFragments.isEmpty())
                  return QVector<Note*>();
            const bool preserveChainTopology = snapshotValue.sourceFragments.size() > 1;
            if (preserveChainTopology) {
                  const int fragmentCount = snapshotValue.sourceFragments.size();
                  if (fragmentCount != added.size()
                      || snapshotValue.fragmentOffsets.size() != fragmentCount
                      || snapshotValue.fragmentDurations.size() != fragmentCount
                      || snapshotValue.fragmentUserEvents.size() != fragmentCount
                      || snapshotValue.sourceTies.size() != fragmentCount)
                        return QVector<Note*>();
                  for (int i = 0; i < fragmentCount; ++i) {
                        if (!added[i] || !added[i]->chord()
                            || added[i]->tick().ticks() - start.ticks()
                                  != snapshotValue.fragmentOffsets[i]
                            || added[i]->chord()->actualTicks().ticks()
                                  != snapshotValue.fragmentDurations[i])
                              return QVector<Note*>();
                        if (i + 1 < fragmentCount
                            && (!added[i]->tieFor() || !snapshotValue.sourceTies[i]))
                              return QVector<Note*>();
                        }
                  }
            const int cloneCount = qMin(snapshotValue.sourceFragments.size(), added.size());
            for (int i = 0; i < cloneCount; ++i) {
                  Note* placeholder = added[i];
                  Note* replacement = new Note(*snapshotValue.sourceFragments[i], false);
                  if (Tie* copiedTie = replacement->tieFor()) {
                        replacement->setTieFor(nullptr);
                        delete copiedTie;
                        }
                  replacement->setTieBack(nullptr);
                  replacement->setParent(placeholder->chord());
                  replacement->setTrack(placeholder->track());
                  replacement->setSelected(false);
                  if (replacement->pitch() != snapshotValue.pitch) {
                        replacement->setPitch(snapshotValue.pitch,
                              replacement->tpc1default(snapshotValue.pitch),
                              replacement->tpc2default(snapshotValue.pitch));
                        }
                  Tie* tieBack = placeholder->tieBack();
                  Tie* tieFor = placeholder->tieFor();
                  _score->undoAddElement(replacement);
                  if (tieBack)
                        _score->undoChangeSpannerElements(tieBack, tieBack->startNote(), replacement);
                  if (tieFor)
                        _score->undoChangeSpannerElements(tieFor, replacement, tieFor->endNote());
                  _score->undoRemoveElement(placeholder);
                  added[i] = replacement;
                  }

            if (preserveChainTopology) {
                  for (int i = 0; i + 1 < added.size(); ++i) {
                        Tie* generated = added[i]->tieFor();
                        Tie* sourceTie = snapshotValue.sourceTies[i];
                        Tie* replacementTie = new Tie(*sourceTie);
                        replacementTie->setStartNote(added[i]);
                        replacementTie->setEndNote(added[i + 1]);
                        replacementTie->setTrack(added[i]->track());
                        replacementTie->setTrack2(added[i + 1]->track());
                        replacementTie->setTick(added[i]->tick());
                        replacementTie->setTicks(added[i + 1]->tick() - added[i]->tick());
                        _score->undoRemoveElement(generated);
                        _score->undoAddElement(replacementTie);
                        }
                  }
            }

      auto persistUserEvents = [](Note* destination, const NoteEventList* requestedEvents) {
            if (!destination)
                  return;
            QSet<Note*> seen;
            for (ScoreElement* linkedElement : destination->linkList()) {
                  if (!linkedElement || linkedElement->type() != ElementType::NOTE)
                        continue;
                  Note* linkedNote = toNote(linkedElement);
                  if (!linkedNote->score() || !linkedNote->chord() || seen.contains(linkedNote))
                        continue;
                  seen.insert(linkedNote);
                  NoteEventList events = requestedEvents
                                       ? *requestedEvents : linkedNote->playEvents();
                  linkedNote->score()->undo(new ChangeNoteEventList(linkedNote, events));
                  }
            };

      if (!added.isEmpty()) {
            if (snapshotValue.source) {
                  const int modeCount = qMin(added.size(), snapshotValue.fragmentUserEvents.size());
                  for (int i = 0; i < modeCount; ++i) {
                        if (snapshotValue.fragmentUserEvents[i])
                              persistUserEvents(added[i], nullptr);
                        }
                  }
            else if (snapshotValue.userEvents) {
                  NoteEventList events;
                  for (const EventSnapshot& eventValue : snapshotValue.events) {
                        NoteEvent event;
                        event.setOntime(eventValue.ontime);
                        event.setLen(eventValue.length);
                        event.setPitch(eventValue.pitch);
                        events.append(event);
                        }
                  persistUserEvents(added.front(), &events);
                  }
            }
      return added;
      }

void KeyEditorModel::deleteRoots(const QList<Note*>& roots)
      {
      if (!_score)
            return;
      QSet<Note*> toDelete;
      for (Note* original : roots) {
            Note* note = rootNote(original);
            while (note) {
                  if (toDelete.contains(note))
                        break;
                  toDelete.insert(note);
                  note = note->tieFor() ? note->tieFor()->endNote() : nullptr;
                  }
            }
      for (Note* note : qAsConst(toDelete))
            _score->deleteItem(note);
      }

void KeyEditorModel::selectCreated(const QVector<Note*>& created)
      {
      if (!_score)
            return;
      Selection& selection = _score->selection();
      selection.deselectAll();
      QSet<Note*> roots;
      for (Note* note : created) {
            note = rootNote(note);
            if (note && !roots.contains(note)) {
                  roots.insert(note);
                  selection.add(note);
                  _score->addRefresh(note->canvasBoundingRect());
                  }
            }
      }

bool KeyEditorModel::applyNoteEdits(const QVector<NoteEdit>& edits, bool duplicate)
      {
      if (!_score || edits.isEmpty())
            return false;
      QVector<NoteSnapshot> values;
      QList<Note*> roots;
      QSet<Note*> seen;
      bool changed = duplicate;
      bool structuralChange = duplicate;
      for (const NoteEdit& edit : edits) {
            Note* root = rootNote(edit.source);
            if (!root || seen.contains(root))
                  continue;
            seen.insert(root);
            NoteSnapshot value = snapshot(root);
            if (!value.source)
                  continue;
            const int newStart = qMax(0, edit.startTick);
            const int newEnd = qMax(newStart + 1, edit.endTick);
            const int newPitch = qBound(0, edit.pitch, 127);
            const int newVoice = qBound(0, edit.voice, VOICES - 1);
            changed = changed || value.startTick != newStart
                    || value.durationTicks != newEnd - newStart
                    || value.pitch != newPitch || value.staffIdx != edit.staffIdx
                    || value.voice != newVoice;
            structuralChange = structuralChange || value.startTick != newStart
                    || value.durationTicks != newEnd - newStart
                    || value.staffIdx != edit.staffIdx || value.voice != newVoice;
            value.startTick = newStart;
            value.durationTicks = newEnd - newStart;
            value.pitch = newPitch;
            value.staffIdx = edit.staffIdx;
            value.voice = newVoice;
            values.append(value);
            roots.append(root);
            }
      if (!changed || values.isEmpty())
            return false;

      // A pitch edit is not a rhythmic rewrite.  Keep the original Note,
      // chord, ties, note children and linked-score identity intact.
      if (!structuralChange) {
            _score->startCmd();
            for (int i = 0; i < values.size(); ++i) {
                  Note* note = rootNote(roots[i]);
                  QSet<Note*> chainSeen;
                  while (note && !chainSeen.contains(note)) {
                        chainSeen.insert(note);
                        if (note->pitch() != values[i].pitch) {
                              _score->undoChangePitch(note, values[i].pitch,
                                    note->tpc1default(values[i].pitch),
                                    note->tpc2default(values[i].pitch));
                              }
                        note = note->tieFor() ? note->tieFor()->endNote() : nullptr;
                        }
                  }
            _score->endCmd();
            rebuild();
            emit selectionChanged();
            return true;
            }

      QSet<Note*> removal;
      if (!duplicate) {
            for (Note* root : qAsConst(roots))
                  removal.unite(tieChain(root));
            }
      for (Note* root : qAsConst(roots)) {
            if (!sourceCanBeRewritten(root, removal))
                  return false;
            }
      for (int i = 0; i < values.size(); ++i) {
            for (int j = 0; j < i; ++j) {
                  if (values[i].staffIdx != values[j].staffIdx
                      || values[i].voice != values[j].voice)
                        continue;
                  const int endI = values[i].startTick + values[i].durationTicks;
                  const int endJ = values[j].startTick + values[j].durationTicks;
                  if (values[i].startTick >= endJ || values[j].startTick >= endI)
                        continue;
                  if (values[i].pitch == values[j].pitch
                      || values[i].startTick != values[j].startTick || endI != endJ
                      || values[i].userEvents != values[j].userEvents
                      || values[i].fragmentUserEvents != values[j].fragmentUserEvents)
                        return false;
                  }
            }
      for (const NoteSnapshot& value : qAsConst(values)) {
            if (!canPlace(value, removal))
                  return false;
            }

      _score->startCmd();
      if (!duplicate)
            deleteRoots(roots);
      QVector<Note*> created;
      for (const NoteSnapshot& value : qAsConst(values)) {
            QVector<Note*> group = addNote(value);
            Note* root = group.isEmpty() ? nullptr : rootNote(group.front());
            if (!root || root->tick().ticks() != value.startTick
                || root->playTicks() != value.durationTicks) {
                  _score->endCmd(true);
                  rebuild();
                  return false;
                  }
            created += group;
            }
      selectCreated(created);
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::applyPlaybackTimingEdits(const QVector<NoteEdit>& edits, bool duplicate)
      {
      if (!_score || edits.isEmpty())
            return false;
      QHash<Note*, NoteEventList> replacements;
      QHash<Note*, QVector<int> > addedIndexes;
      bool changed = false;
      for (const NoteEdit& edit : edits) {
            Note* source = edit.source;
            if (!source || !source->chord() || edit.eventIndex < 0
                || edit.eventIndex >= source->playEvents().size())
                  continue;
            if (edit.staffIdx != source->staffIdx() || edit.voice != source->voice())
                  return false;
            const int desiredStart = qMax(0, edit.startTick);
            const int desiredEnd = qMax(desiredStart + 1, edit.endTick);
            Chord* anchor = playbackAnchor(source);
            if (!anchor)
                  continue;
            const int rootTicks = qMax(1, anchor->actualTicks().ticks());
            const int ontime = qRound(qreal(desiredStart - anchor->tick().ticks())
                                      * NoteEvent::NOTE_LENGTH / rootTicks);
            // The renderer appends simple tied continuations to the final
            // event automatically. The block's visible duration already
            // contains that tail, so do not write it into the source event a
            // second time or every resize compounds the tie duration.
            const int editableTicks = qMax(1, desiredEnd - desiredStart
                                              - playbackTieTail(source, edit.eventIndex));
            const int length = qMax(1, qRound(qreal(editableTicks)
                                              * NoteEvent::NOTE_LENGTH / rootTicks));
            NoteEventList list = replacements.contains(source)
                               ? replacements.value(source) : source->playEvents();
            NoteEvent event = list[edit.eventIndex];
            const int pitchOffset = qBound(-127, edit.pitch - source->ppitch(), 127);
            if (duplicate) {
                  event.setOntime(ontime);
                  event.setLen(length);
                  event.setPitch(pitchOffset);
                  addedIndexes[source].append(list.size());
                  list.append(event);
                  changed = true;
                  }
            else if (event.ontime() != ontime || event.len() != length
                     || event.pitch() != pitchOffset) {
                  event.setOntime(ontime);
                  event.setLen(length);
                  event.setPitch(pitchOffset);
                  list[edit.eventIndex] = event;
                  changed = true;
                  }
            replacements.insert(source, list);
            }
      if (!changed || replacements.isEmpty())
            return false;
      _score->startCmd();
      for (auto it = replacements.begin(); it != replacements.end(); ++it) {
            undoEventListForLinkedNotes(it.key(), it.value());
            }
      _score->endCmd();
      rebuild();
      if (duplicate) {
            _selectedEvents.clear();
            for (auto it = addedIndexes.constBegin(); it != addedIndexes.constEnd(); ++it) {
                  for (int eventIndex : it.value())
                        _selectedEvents.insert(eventKey(it.key(), eventIndex));
                  }
            }
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::createNote(int startTick, int durationTicks, int pitch, int staffIdx, int voice)
      {
      if (!_score || !_editStaff || durationTicks <= 0)
            return false;
      NoteSnapshot value;
      value.startTick = qMax(0, startTick);
      value.durationTicks = qMax(1, durationTicks);
      value.pitch = qBound(0, pitch, 127);
      value.staffIdx = staffIdx;
      value.voice = qBound(0, voice, VOICES - 1);
      value.velocity = 0;
      value.velocityType = int(Note::ValueType::OFFSET_VAL);
      if (!canPlace(value, QSet<Note*>()))
            return false;
      _score->startCmd();
      QVector<Note*> created = addNote(value);
      Note* root = created.isEmpty() ? nullptr : rootNote(created.front());
      if (!root || root->tick().ticks() != value.startTick
          || root->playTicks() != value.durationTicks) {
            _score->endCmd(true);
            rebuild();
            return false;
            }
      selectCreated(created);
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return !created.isEmpty();
      }

bool KeyEditorModel::deleteNotes(const QList<Note*>& notes)
      {
      if (!_score || notes.isEmpty())
            return false;
      _score->startCmd();
      deleteRoots(notes);
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::deleteSelection()
      {
      if (!_score)
            return false;
      QHash<Note*, QVector<int> > removals;
      for (int blockIndex : selectedEventIndexes()) {
            const NoteBlock& block = _notes[blockIndex];
            if (block.note && block.eventIndex >= 0)
                  removals[block.note].append(block.eventIndex);
            }
      if (removals.isEmpty())
            return false;
      _score->startCmd();
      for (auto it = removals.begin(); it != removals.end(); ++it) {
            NoteEventList list = it.key()->playEvents();
            QVector<int> indexes = it.value();
            std::sort(indexes.begin(), indexes.end(), std::greater<int>());
            indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
            for (int index : qAsConst(indexes)) {
                  if (index >= 0 && index < list.size())
                        list.removeAt(index);
                  }
            undoEventListForLinkedNotes(it.key(), list);
            }
      _selectedEvents.clear();
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::setSelectionVoice(int voice)
      {
      if (!_score)
            return false;
      voice = qBound(0, voice, VOICES - 1);
      const QList<Note*> roots = selectedRootNotes();
      bool changed = false;
      for (Note* root : roots)
            changed = changed || root->voice() != voice;
      if (!changed)
            return false;

      Selection& selection = _score->selection();
      selection.deselectAll();
      for (Note* root : roots) {
            for (Note* note : tieChain(root))
                  selection.add(note);
            }
      _score->changeVoice(voice);
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::setVelocities(const QHash<Note*, int>& velocities)
      {
      if (!_score || velocities.isEmpty())
            return false;
      QHash<Note*, int> clean;
      for (auto it = velocities.constBegin(); it != velocities.constEnd(); ++it) {
            Note* note = rootNote(it.key());
            if (note && staffIsVisible(note->staffIdx()))
                  clean.insert(note, qBound(1, it.value(), 127));
            }
      bool changed = false;
      for (auto it = clean.constBegin(); it != clean.constEnd(); ++it)
            changed = changed || it.key()->veloType() != Note::ValueType::USER_VAL
                    || it.key()->veloOffset() != it.value();
      if (!changed)
            return false;
      _score->startCmd();
      for (auto it = clean.constBegin(); it != clean.constEnd(); ++it) {
            if (it.key()->veloType() == Note::ValueType::USER_VAL
                && it.key()->veloOffset() == it.value())
                  continue;
            _score->undo(new ChangeVelocity(it.key(), Note::ValueType::USER_VAL, it.value()));
            }
      _score->endCmd();
      rebuild();
      return true;
      }

bool KeyEditorModel::setEventVelocities(const QHash<int, int>& velocities)
      {
      if (!_score || velocities.isEmpty())
            return false;
      QHash<Note*, NoteEventList> replacements;
      bool changed = false;
      for (auto it = velocities.constBegin(); it != velocities.constEnd(); ++it) {
            const int blockIndex = it.key();
            if (blockIndex < 0 || blockIndex >= _notes.size())
                  continue;
            const NoteBlock& block = _notes[blockIndex];
            Note* note = block.note;
            if (!note || block.eventIndex < 0 || block.eventIndex >= note->playEvents().size())
                  continue;
            NoteEventList list = replacements.contains(note)
                               ? replacements.value(note) : note->playEvents();
            const int value = qBound(1, it.value(), 127);
            if (list[block.eventIndex].velocity() != value) {
                  list[block.eventIndex].setVelocity(value);
                  changed = true;
                  }
            replacements.insert(note, list);
            }
      if (!changed)
            return false;
      _score->startCmd();
      for (auto it = replacements.begin(); it != replacements.end(); ++it) {
            undoEventListForLinkedNotes(it.key(), it.value());
            }
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::resetSelectionVelocities()
      {
      if (!_score)
            return false;
      const QVector<int> selected = selectedEventIndexes();
      if (selected.isEmpty())
            return false;

      QHash<Note*, NoteEventList> replacements;
      QSet<Note*> sourceNotes;
      bool rawVelocityChanged = false;
      bool notationVelocityChanged = false;
      for (int blockIndex : selected) {
            if (blockIndex < 0 || blockIndex >= _notes.size())
                  continue;
            const NoteBlock& block = _notes[blockIndex];
            Note* note = block.note;
            if (!note || block.eventIndex < 0 || block.eventIndex >= note->playEvents().size())
                  continue;

            sourceNotes.insert(note);
            notationVelocityChanged = notationVelocityChanged
                  || note->veloType() != Note::ValueType::OFFSET_VAL
                  || note->veloOffset() != 0;
            NoteEventList list = replacements.contains(note)
                               ? replacements.value(note) : note->playEvents();
            if (list[block.eventIndex].velocity() >= 0) {
                  list[block.eventIndex].setVelocity(-1);
                  rawVelocityChanged = true;
                  }
            replacements.insert(note, list);
            }

      if (!rawVelocityChanged && !notationVelocityChanged)
            return false;

      _score->startCmd();
      if (rawVelocityChanged) {
            for (auto it = replacements.begin(); it != replacements.end(); ++it)
                  undoEventListForLinkedNotes(it.key(), it.value());
            }
      if (notationVelocityChanged) {
            for (Note* note : sourceNotes) {
                  if (note->veloType() == Note::ValueType::OFFSET_VAL && note->veloOffset() == 0)
                        continue;
                  undoVelocityForLinkedNotes(note, Note::ValueType::OFFSET_VAL, 0);
                  }
            }
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::setSelectionEventTiming(int value, bool changeOntime)
      {
      if (!_score)
            return false;
      const QVector<int> selected = selectedEventIndexes();
      if (selected.isEmpty())
            return false;
      QHash<Note*, NoteEventList> replacements;
      bool changed = false;
      for (int blockIndex : selected) {
            const NoteBlock& block = _notes[blockIndex];
            Note* note = block.note;
            if (!note || block.eventIndex < 0 || block.eventIndex >= note->playEvents().size())
                  continue;
            NoteEventList list = replacements.contains(note)
                               ? replacements.value(note) : note->playEvents();
            NoteEvent& event = list[block.eventIndex];
            if (changeOntime) {
                  if (event.ontime() == value)
                        continue;
                  event.setOntime(value);
                  }
            else {
                  const int length = qMax(1, value);
                  if (event.len() == length)
                        continue;
                  event.setLen(length);
                  }
            replacements.insert(note, list);
            changed = true;
            }
      if (!changed)
            return false;
      _score->startCmd();
      for (auto it = replacements.begin(); it != replacements.end(); ++it) {
            undoEventListForLinkedNotes(it.key(), it.value());
            }
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::setNoteEventTiming(Note* note, int eventIndex, int value, bool changeOntime)
      {
      if (!_score || !note || eventIndex < 0 || eventIndex >= note->playEvents().size())
            return false;
      NoteEvent* event = &note->playEvents()[eventIndex];
      NoteEvent replacement = *event;
      if (changeOntime) {
            if (event->ontime() == value)
                  return false;
            replacement.setOntime(value);
            }
      else {
            value = qMax(1, value);
            if (event->len() == value)
                  return false;
            replacement.setLen(value);
            }
      _score->startCmd();
      _score->undo(new ChangeNoteEvent(note, event, replacement));
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return true;
      }

bool KeyEditorModel::setSelectionVelocity(int velocity)
      {
      QHash<int, int> values;
      for (int index : selectedEventIndexes())
            values.insert(index, velocity);
      return setEventVelocities(values);
      }

bool KeyEditorModel::quantizeSelection(int gridTicks)
      {
      if (gridTicks <= 0)
            return false;
      QVector<NoteEdit> edits;
      for (int index : selectedEventIndexes()) {
            const NoteBlock& block = _notes[index];
            const int start = qMax(0, int(std::floor((block.startTick + gridTicks / 2.0) / gridTicks)) * gridTicks);
            edits.append({ block.note, start, start + (block.endTick - block.startTick), block.pitch,
                           block.staffIdx, block.voice, block.eventIndex });
            }
      return applyPlaybackTimingEdits(edits);
      }

bool KeyEditorModel::nudgeSelection(int tickDelta, int pitchDelta, bool duplicate)
      {
      QVector<NoteEdit> edits;
      int minimumStart = INT_MAX;
      for (int index : selectedEventIndexes()) {
            const NoteBlock& block = _notes[index];
            minimumStart = qMin(minimumStart, block.startTick);
            edits.append({ block.note, block.startTick + tickDelta, block.endTick + tickDelta,
                           block.pitch + pitchDelta, block.staffIdx, block.voice, block.eventIndex });
            }
      if (minimumStart != INT_MAX && minimumStart + tickDelta < 0) {
            const int correction = -(minimumStart + tickDelta);
            for (NoteEdit& edit : edits) {
                  edit.startTick += correction;
                  edit.endTick += correction;
                  }
            }
      return applyPlaybackTimingEdits(edits, duplicate);
      }

QByteArray KeyEditorModel::encodeSnapshots(const QVector<NoteSnapshot>& snapshots) const
      {
      QByteArray bytes;
      QDataStream stream(&bytes, QIODevice::WriteOnly);
      stream.setVersion(QDataStream::Qt_5_9);
      stream << KEY_EDITOR_CLIPBOARD_MAGIC << KEY_EDITOR_CLIPBOARD_VERSION;
      stream << quint32(snapshots.size());
      for (const NoteSnapshot& value : snapshots) {
            stream << qint32(value.startTick) << qint32(value.durationTicks)
                   << qint32(value.pitch) << qint32(value.staffIdx) << qint32(value.voice)
                   << qint32(value.velocity) << qint32(value.velocityType)
                   << value.tuning << qint32(value.subchannel)
                   << quint8(value.play ? 1 : 0)
                   << quint8(value.userEvents ? 1 : 0)
                   << quint32(value.events.size());
            for (const EventSnapshot& event : value.events)
                  stream << qint32(event.ontime) << qint32(event.length) << qint32(event.pitch);
            }
      return bytes;
      }

QVector<KeyEditorModel::NoteSnapshot> KeyEditorModel::decodeSnapshots(const QByteArray& bytes) const
      {
      QVector<NoteSnapshot> result;
      QDataStream stream(bytes);
      stream.setVersion(QDataStream::Qt_5_9);
      quint32 magic = 0;
      quint16 version = 0;
      quint32 count = 0;
      stream >> magic >> version >> count;
      if (magic != KEY_EDITOR_CLIPBOARD_MAGIC || version != KEY_EDITOR_CLIPBOARD_VERSION || count > 100000)
            return result;
      result.reserve(int(count));
      for (quint32 i = 0; i < count && stream.status() == QDataStream::Ok; ++i) {
            NoteSnapshot value;
            qint32 start, duration, pitch, staffIdx, voice, velocity, velocityType;
            qreal tuning;
            qint32 subchannel;
            quint8 play;
            quint8 userEvents;
            quint32 eventCount;
            stream >> start >> duration >> pitch >> staffIdx >> voice >> velocity >> velocityType
                   >> tuning >> subchannel >> play >> userEvents >> eventCount;
            if (eventCount > 1024)
                  return QVector<NoteSnapshot>();
            value.startTick = start;
            value.durationTicks = qMax(1, int(duration));
            value.pitch = qBound(0, int(pitch), 127);
            value.staffIdx = staffIdx;
            value.voice = qBound(0, int(voice), VOICES - 1);
            value.velocity = velocity;
            value.velocityType = velocityType;
            value.tuning = tuning;
            value.subchannel = qBound(0, int(subchannel), 127);
            value.play = play != 0;
            value.userEvents = userEvents != 0;
            for (quint32 eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                  qint32 ontime, length, eventPitch;
                  stream >> ontime >> length >> eventPitch;
                  value.events.append({ int(ontime), int(length), int(eventPitch) });
                  }
            result.append(value);
            }
      if (stream.status() != QDataStream::Ok)
            result.clear();
      return result;
      }

void KeyEditorModel::copySelection() const
      {
      const QVector<NoteSnapshot> values = selectedSnapshots();
      if (values.isEmpty())
            return;
      QMimeData* mime = new QMimeData;
      mime->setData(KEY_EDITOR_MIME, encodeSnapshots(values));
      QApplication::clipboard()->setMimeData(mime);
      }

bool KeyEditorModel::cutSelection()
      {
      if (selectedRootNotes().isEmpty())
            return false;
      copySelection();
      return deleteSelection();
      }

bool KeyEditorModel::pasteAt(int tick, int targetStaffIdx)
      {
      if (!_score || !_editStaff)
            return false;
      const QMimeData* mime = QApplication::clipboard()->mimeData();
      if (!mime || !mime->hasFormat(KEY_EDITOR_MIME))
            return false;
      QVector<NoteSnapshot> values = decodeSnapshots(mime->data(KEY_EDITOR_MIME));
      if (values.isEmpty())
            return false;
      int firstTick = INT_MAX;
      for (const NoteSnapshot& value : qAsConst(values))
            firstTick = qMin(firstTick, value.startTick);
      for (NoteSnapshot& value : values) {
            value.source = nullptr;
            value.startTick = qMax(0, tick + value.startTick - firstTick);
            Staff* sourceStaff = _score->staff(value.staffIdx);
            if (targetStaffIdx >= 0)
                  value.staffIdx = targetStaffIdx;
            else if (!sourceStaff || sourceStaff->part() != _editStaff->part())
                  value.staffIdx = _editStaff->idx();
            }
      const QSet<Note*> noRemoval;
      for (int i = 0; i < values.size(); ++i) {
            if (!canPlace(values[i], noRemoval))
                  return false;
            for (int j = 0; j < i; ++j) {
                  const bool sameTrack = values[i].staffIdx == values[j].staffIdx
                                      && values[i].voice == values[j].voice;
                  const bool samePitch = values[i].pitch == values[j].pitch;
                  const int endI = values[i].startTick + values[i].durationTicks;
                  const int endJ = values[j].startTick + values[j].durationTicks;
                  if (sameTrack && values[i].startTick < endJ && values[j].startTick < endI) {
                        if (samePitch || values[i].startTick != values[j].startTick || endI != endJ
                            || values[i].userEvents != values[j].userEvents)
                              return false;
                        }
                  }
            }
      _score->startCmd();
      QVector<Note*> created;
      for (const NoteSnapshot& value : qAsConst(values)) {
            QVector<Note*> group = addNote(value);
            Note* root = group.isEmpty() ? nullptr : rootNote(group.front());
            if (!root || root->tick().ticks() != value.startTick
                || root->playTicks() != value.durationTicks) {
                  _score->endCmd(true);
                  rebuild();
                  return false;
                  }
            created += group;
            }
      selectCreated(created);
      _score->endCmd();
      rebuild();
      emit selectionChanged();
      return !created.isEmpty();
      }

bool KeyEditorModel::createPedal(int startTick, int endTick, int staffIdx)
      {
      if (!_score)
            return false;
      Staff* staff = _score->staff(staffIdx);
      if (!staff || !_editStaff || staff->part() != _editStaff->part())
            staff = _editStaff;
      if (!staff)
            return false;
      startTick = qMax(0, startTick);
      endTick = qMin(_scoreEndTick, qMax(startTick + 1, endTick));
      if (startTick >= _scoreEndTick || endTick <= startTick)
            return false;
      for (const PedalBlock& existing : _pedals) {
            if (existing.staffIdx == staff->idx() && startTick < existing.endTick
                && existing.startTick < endTick)
                  return false;
            }
      Pedal* pedal = new Pedal(_score);
      const int track = staff->idx() * VOICES;
      pedal->setTrack(track);
      pedal->setTrack2(track);
      pedal->setTick(Fraction::fromTicks(startTick));
      pedal->setTicks(Fraction::fromTicks(endTick - startTick));
      _score->startCmd();
      _score->undoAddElement(pedal);
      _score->endCmd();
      rebuild();
      return true;
      }

bool KeyEditorModel::editPedal(Spanner* spanner, int startTick, int endTick)
      {
      if (!_score || !spanner || (!spanner->isPedal() && !spanner->isLetRing()))
            return false;
      startTick = qMax(0, startTick);
      endTick = qMin(_scoreEndTick, qMax(startTick + 1, endTick));
      if (startTick >= _scoreEndTick || endTick <= startTick)
            return false;
      for (const PedalBlock& existing : _pedals) {
            if (existing.spanner != spanner && existing.staffIdx == spanner->staffIdx()
                && startTick < existing.endTick && existing.startTick < endTick)
                  return false;
            }
      if (spanner->tick().ticks() == startTick && spanner->tick2().ticks() == endTick)
            return false;
      _score->startCmd();
      spanner->undoChangeProperty(Pid::SPANNER_TICK, Fraction::fromTicks(startTick));
      spanner->undoChangeProperty(Pid::SPANNER_TICKS, Fraction::fromTicks(endTick - startTick));
      _score->endCmd();
      rebuild();
      return true;
      }

bool KeyEditorModel::deletePedal(Spanner* spanner)
      {
      if (!_score || !spanner || (!spanner->isPedal() && !spanner->isLetRing()))
            return false;
      _score->startCmd();
      _score->undoRemoveElement(spanner);
      _score->endCmd();
      rebuild();
      return true;
      }

} // namespace Ms
