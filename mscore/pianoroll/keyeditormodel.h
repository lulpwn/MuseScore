//=============================================================================
//  MuseScore
//  Key Editor model
//
//  This file is part of MuseScore.
//  MuseScore is free software: you can redistribute it and/or modify it under
//  the terms of the GNU General Public License version 2.
//=============================================================================

#ifndef __KEYEDITOR_MODEL_H__
#define __KEYEDITOR_MODEL_H__

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

namespace Ms {

class ChordRest;
class Chord;
class Element;
class Measure;
class Note;
class Score;
class Segment;
class Spanner;
class Staff;
class Tie;

class KeyEditorModel : public QObject
      {
      Q_OBJECT

   public:
      enum class SelectionOperation : char {
            Replace,
            Add,
            Toggle,
            Subtract
            };

      struct NoteBlock {
            Note* note { nullptr };
            int startTick { 0 };
            int endTick { 1 };
            int pitch { 60 };
            int staffIdx { 0 };
            int voice { 0 };
            int velocity { 80 };
            int eventIndex { -1 };
            int ontime { 0 };
            int eventLength { 1000 };
            int pitchOffset { 0 };
            bool overlap { false };
            bool grace { false };
            };

      struct PedalBlock {
            Spanner* spanner { nullptr };
            int startTick { 0 };
            int endTick { 1 };
            int staffIdx { 0 };
            int sourceStartTick { -1 };
            int sourceEndTick { -1 };
            bool letRing { false };
            };

      struct NoteEdit {
            Note* source { nullptr };
            int startTick { 0 };
            int endTick { 1 };
            int pitch { 60 };
            int staffIdx { 0 };
            int voice { 0 };
            int eventIndex { -1 };
            };

   private:
      struct EventSnapshot {
            int ontime { 0 };
            int length { 1000 };
            int pitch { 0 };
            bool suppressTieTail { false };
            };

      struct NoteSnapshot {
            Note* source { nullptr };
            QVector<Note*> sourceFragments;
            QVector<Tie*> sourceTies;
            QVector<int> fragmentOffsets;
            QVector<int> fragmentDurations;
            QVector<bool> fragmentUserEvents;
            int startTick { 0 };
            int durationTicks { 1 };
            int pitch { 60 };
            int staffIdx { 0 };
            int voice { 0 };
            int velocity { 0 };
            int velocityType { 0 };
            qreal tuning { 0.0 };
            int subchannel { 0 };
            bool play { true };
            bool userEvents { false };
            QVector<EventSnapshot> events;
            };

      Score* _score { nullptr };
      Staff* _editStaff { nullptr };
      QList<Staff*> _visibleStaves;
      QVector<NoteBlock> _notes;
      QVector<PedalBlock> _pedals;
      QHash<int, QVector<int> > _timeBuckets;
      QHash<Note*, int> _noteLookup;
      QSet<QString> _selectedEvents;
      int _scoreEndTick { 0 };
      int _bucketTicks { 1920 };
      bool _rebuilding { false };
      bool _projectionUpdatesSuspended { false };
      bool _projectionRebuildPending { false };

      Note* rootNote(Note*) const;
      QString eventKey(Note*, int eventIndex) const;
      Chord* playbackAnchor(Note*) const;
      QSet<Note*> tieChain(Note*) const;
      bool playbackBounds(Note*, int& startTick, int& endTick) const;
      NoteSnapshot snapshot(Note*) const;
      bool usesUserEventsAt(const NoteSnapshot&, int relativeTick) const;
      QList<Note*> selectedRootNotes() const;
      QVector<Note*> segmentNotes(Segment*, int track) const;
      QVector<Note*> addNote(const NoteSnapshot&);
      bool canPlace(const NoteSnapshot&, const QSet<Note*>& removal) const;
      bool sourceCanBeRewritten(Note*, const QSet<Note*>& removal) const;
      bool cutChordRest(ChordRest*, int track, int cutTick, ChordRest*&, ChordRest*&);
      void addTie(Note*);
      void deleteRoots(const QList<Note*>&);
      int effectiveVelocityInternal(Note*) const;
      void rebuildIndexes();
      void rebuildPedals();
      bool staffIsVisible(int) const;
      QByteArray encodeSnapshots(const QVector<NoteSnapshot>&) const;
      QVector<NoteSnapshot> decodeSnapshots(const QByteArray&) const;
      QVector<NoteSnapshot> selectedSnapshots() const;
      void selectCreated(const QVector<Note*>&);

   signals:
      void modelReset();
      void selectionChanged();

   public:
      explicit KeyEditorModel(QObject* parent = nullptr);

      void setContext(Staff* editStaff, const QList<Staff*>& visibleStaves);
      void clear();
      void invalidateProjection();
      void invalidateElement(Element*);
      void setProjectionUpdatesSuspended(bool);
      void rebuild();
      void syncSelection();

      Score* score() const                         { return _score; }
      Staff* editStaff() const                     { return _editStaff; }
      const QList<Staff*>& visibleStaves() const   { return _visibleStaves; }
      const QVector<NoteBlock>& notes() const      { return _notes; }
      const QVector<PedalBlock>& pedals() const    { return _pedals; }
      int scoreEndTick() const                     { return _scoreEndTick; }

      QVector<int> notesInRange(int startTick, int endTick, int lowPitch, int highPitch) const;
      int noteAt(int tick, int pitch) const;
      int noteIndex(Note*) const;
      int noteIndex(Note*, int eventIndex) const;
      bool eventSelected(int noteIndex) const;
      bool eventSelected(Note*, int eventIndex) const;
      bool noteSelected(Note*) const;
      QVector<int> selectedEventIndexes() const;
      QList<Note*> selectedNotes() const;
      int effectiveVelocity(Note*) const;

      void select(const QList<Note*>&, SelectionOperation);
      void selectEvents(const QVector<int>&, SelectionOperation);
      void selectRange(int startTick, int endTick, int lowPitch, int highPitch,
                       SelectionOperation operation);
      void selectAllVisible();
      void clearSelection();

      bool applyNoteEdits(const QVector<NoteEdit>&, bool duplicate);
      bool applyPlaybackTimingEdits(const QVector<NoteEdit>&, bool duplicate = false);
      bool createNote(int startTick, int durationTicks, int pitch, int staffIdx, int voice);
      bool deleteSelection();
      bool deleteNotes(const QList<Note*>&);
      bool setSelectionVoice(int voice);
      bool setSelectionVelocity(int velocity);
      bool setVelocities(const QHash<Note*, int>& velocities);
      bool setEventVelocities(const QHash<int, int>& velocities);
      bool resetSelectionVelocities();
      bool setSelectionEventTiming(int value, bool changeOntime);
      bool setNoteEventTiming(Note*, int eventIndex, int value, bool changeOntime);
      bool quantizeSelection(int gridTicks);
      bool nudgeSelection(int tickDelta, int pitchDelta, bool duplicate);

      void copySelection() const;
      bool cutSelection();
      bool pasteAt(int tick, int targetStaffIdx = -1);

      bool createPedal(int startTick, int endTick, int staffIdx);
      bool editPedal(int staffIdx, int oldStartTick, int oldEndTick,
                     int startTick, int endTick);
      bool deletePedal(int staffIdx, int startTick, int endTick);

      };

} // namespace Ms

#endif
