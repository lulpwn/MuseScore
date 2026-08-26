//=============================================================================
//  MuseScore
//  Key Editor regression tests
//=============================================================================

#include <QtTest/QtTest>

#include "mtest/testutils.h"
#include "mscore/pianoroll/keyeditormodel.h"
#include "mscore/pianoroll/keyeditorview.h"
#include "mscore/pianoroll/pianoroll.h"

#include "libmscore/note.h"
#include "libmscore/noteevent.h"
#include "libmscore/chord.h"
#include "libmscore/part.h"
#include "libmscore/pedal.h"
#include "libmscore/playbacksustain.h"
#include "libmscore/score.h"
#include "libmscore/staff.h"

#include <QImage>
#include <QPainter>
#include <QComboBox>
#include <QScrollBar>
#include <QSet>

using namespace Ms;

class TestPianoRoll : public QObject, public MTest
      {
      Q_OBJECT

      MasterScore* readGrandStaffScore()
            {
            return readScore(QStringLiteral("importmidi/instrument_grand.mscx"));
            }

   private slots:
      void initTestCase() { initMTest(); }
      void allTracksProjectionAndRender();
      void noteTransactionsUndoAndRedo();
      void sustainSpanUndoAndRedo();
      void notationPedalRemainsAuthoritativeAfterPlaybackEdit();
      void projectionInvalidationKeepsViewport();
      void spaceRequestsPlaybackToggle();
      void controllerSelectorLivesInLane();
      void playbackTimingDoesNotChangeNotation();
      void tiedPlaybackResizeDoesNotCompound();
      void renderedEventsAreIndividuallyEditable();
      void notationPitchRebasesPlaybackAdjustment();
      void removingOrnamentRegeneratesEditedPlayback();
      void selectionDoesNotInvalidatePlayback();
      };

void TestPianoRoll::allTracksProjectionAndRender()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      QVERIFY(staff && staff->part() && staff->part()->staves());
      const QList<Staff*> visible = *staff->part()->staves();
      QCOMPARE(visible.size(), 2);

      KeyEditorModel model;
      model.setContext(staff, visible);
      QVERIFY(!model.notes().isEmpty());
      QSet<int> projectedStaves;
      for (const KeyEditorModel::NoteBlock& block : model.notes())
            projectedStaves.insert(block.staffIdx);
      QCOMPARE(projectedStaves.size(), 2);

      KeyEditorView view;
      view.resize(960, 620);
      view.setModel(&model);
      view.ensurePitchVisible(60, true);
      QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      QPainter painter(&image);
      view.render(&painter);
      painter.end();
      QVERIFY(!image.isNull());
      QVERIFY(image.width() == 960 && image.height() == 620);

      delete testScore;
      }

void TestPianoRoll::noteTransactionsUndoAndRedo()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      const QList<Staff*> visible = *staff->part()->staves();
      KeyEditorModel model;
      model.setContext(staff, visible);
      const int originalCount = model.notes().size();

      QVERIFY(model.createNote(0, 120, 65, staff->idx(), 1));
      QCOMPARE(model.notes().size(), originalCount + 1);
      QList<Note*> selected = model.selectedNotes();
      QCOMPARE(selected.size(), 1);
      Note* created = selected.front();

      QHash<Note*, int> velocity;
      velocity.insert(created, 103);
      QVERIFY(model.setVelocities(velocity));
      QCOMPARE(model.effectiveVelocity(model.selectedNotes().front()), 103);
      testScore->undoRedo(true, nullptr);
      model.rebuild();
      QVERIFY(model.effectiveVelocity(model.selectedNotes().front()) != 103);
      testScore->undoRedo(false, nullptr);
      model.rebuild();
      QCOMPARE(model.effectiveVelocity(model.selectedNotes().front()), 103);

      created = model.selectedNotes().front();
      KeyEditorModel::NoteEdit edit;
      edit.source = created;
      edit.startTick = 480;
      edit.endTick = 600;
      edit.pitch = 67;
      edit.staffIdx = staff->idx();
      edit.voice = 1;
      QVERIFY(model.applyNoteEdits({ edit }, false));
      selected = model.selectedNotes();
      QCOMPARE(selected.size(), 1);
      QCOMPARE(selected.front()->tick().ticks(), 480);
      QCOMPARE(selected.front()->pitch(), 67);

      testScore->undoRedo(true, nullptr);
      model.rebuild();
      selected = model.selectedNotes();
      QCOMPARE(selected.size(), 1);
      QCOMPARE(selected.front()->tick().ticks(), 0);
      QCOMPARE(selected.front()->pitch(), 65);
      testScore->undoRedo(false, nullptr);
      model.rebuild();
      QCOMPARE(model.selectedNotes().front()->tick().ticks(), 480);

      delete testScore;
      }

void TestPianoRoll::sustainSpanUndoAndRedo()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      KeyEditorModel model;
      model.setContext(staff, *staff->part()->staves());
      const int originalCount = model.pedals().size();

      QVERIFY(model.createPedal(120, 720, staff->idx()));
      QCOMPARE(model.pedals().size(), originalCount + 1);
      testScore->undoRedo(true, nullptr);
      model.rebuild();
      QCOMPARE(model.pedals().size(), originalCount);
      testScore->undoRedo(false, nullptr);
      model.rebuild();
      QCOMPARE(model.pedals().size(), originalCount + 1);

      delete testScore;
      }

void TestPianoRoll::notationPedalRemainsAuthoritativeAfterPlaybackEdit()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      Pedal* pedal = new Pedal(testScore);
      pedal->setTrack(staff->idx() * VOICES);
      pedal->setTrack2(staff->idx() * VOICES);
      pedal->setTick(Fraction::fromTicks(240));
      pedal->setTicks(Fraction::fromTicks(480));
      testScore->startCmd();
      testScore->undoAddElement(pedal);
      testScore->endCmd();

      KeyEditorModel model;
      model.setContext(staff, *staff->part()->staves());
      const int notationCount = model.pedals().size();
      const KeyEditorModel::PedalBlock* notationBlock = nullptr;
      for (const KeyEditorModel::PedalBlock& block : model.pedals()) {
            if (block.spanner == pedal) {
                  notationBlock = &block;
                  break;
                  }
            }
      QVERIFY(notationBlock);
      const int notationStart = notationBlock->startTick;
      const int notationEnd = notationBlock->endTick;
      QVERIFY(model.editPedal(staff->idx(), notationStart, notationEnd, 300, 700));
      QCOMPARE(pedal->tick().ticks(), notationStart);
      QCOMPARE(pedal->tick2().ticks(), notationEnd);

      QVector<PlaybackSustainSpan> stored;
      QVERIFY(playbackSustainSpans(testScore->synthesizerState(), staff->idx(), &stored));
      QCOMPARE(stored.size(), 1);
      QCOMPARE(stored.front().sourceStartTick, notationStart);
      QCOMPARE(stored.front().sourceEndTick, notationEnd);

      // Removing the notation pedal must also remove its effective CC64 span;
      // the playback-only override must not become an orphan.
      testScore->startCmd();
      testScore->undoRemoveElement(pedal);
      testScore->endCmd();
      model.rebuild();
      QCOMPARE(model.pedals().size(), notationCount - 1);

      delete testScore;
      }

void TestPianoRoll::projectionInvalidationKeepsViewport()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      KeyEditorModel model;
      model.setContext(staff, *staff->part()->staves());
      QVERIFY(!model.notes().isEmpty());

      KeyEditorView view;
      view.resize(520, 420);
      view.setModel(&model);
      view.setHorizontalZoom(1.0);
      view.horizontalScrollBar()->setValue(qMin(100, view.horizontalScrollBar()->maximum()));
      const int beforeScroll = view.horizontalScrollBar()->value();
      const int beforeExtent = model.scoreEndTick();
      const int beforeNotes = model.notes().size();
      QVERIFY(beforeScroll > 0);

      Note* victim = model.notes().front().note;
      model.invalidateElement(victim);
      QCOMPARE(model.scoreEndTick(), beforeExtent);
      QCOMPARE(model.notes().size(), beforeNotes - 1);
      QCOMPARE(view.horizontalScrollBar()->value(), beforeScroll);

      // The element is still alive in this synthetic notification; restore the
      // projection before destroying the score.
      model.rebuild();
      delete testScore;
      }

void TestPianoRoll::spaceRequestsPlaybackToggle()
      {
      KeyEditorView view;
      QSignalSpy requested(&view, SIGNAL(togglePlaybackRequested()));
      QTest::keyClick(&view, Qt::Key_Space);
      QCOMPARE(requested.count(), 1);
      }

void TestPianoRoll::controllerSelectorLivesInLane()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      {
            PianorollEditor editor;
            editor.resize(1000, 700);
            editor.setScore(testScore);
            editor.setStaff(testScore->staff(0));
            editor.show();
            QCoreApplication::processEvents();

            KeyEditorView* view = editor.findChild<KeyEditorView*>();
            QVERIFY(view);
            QComboBox* controller = nullptr;
            for (QComboBox* combo : editor.findChildren<QComboBox*>()) {
                  if (combo->findText(QStringLiteral("Sustain (CC64)")) >= 0) {
                        controller = combo;
                        break;
                        }
                  }
            QVERIFY(controller);
            QCOMPARE(controller->parentWidget(), view->viewport());
            QVERIFY(controller->findText(QStringLiteral("Tempo Map")) >= 0);
            QCOMPARE(controller->findText(QStringLiteral("Position (On-time)")), -1);
            QCOMPARE(controller->findText(QStringLiteral("Duration (Event len)")), -1);
            const QPoint position = controller->mapTo(view->viewport(), QPoint());
            QVERIFY(position.y() >= view->viewport()->height() - view->controllerHeight());

            controller->setCurrentIndex(controller->findText(QStringLiteral("Tempo Map")));
            QCoreApplication::processEvents();
            QStringList labels;
            for (QLabel* label : editor.findChildren<QLabel*>())
                  labels.append(label->text());
            QVERIFY(labels.contains(QStringLiteral("OnTime")));
            QVERIFY(labels.contains(QStringLiteral("Length")));
            QVERIFY(!labels.contains(QStringLiteral("Draw length")));
            QVERIFY(!labels.contains(QStringLiteral("Start")));
            QVERIFY(!labels.contains(QStringLiteral("Pitch")));
            QVERIFY(!labels.contains(QStringLiteral("Voice")));

            QImage image(editor.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            editor.render(&painter);
            painter.end();
            QVERIFY(!image.isNull());
            }
      delete testScore;
      }

void TestPianoRoll::playbackTimingDoesNotChangeNotation()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      KeyEditorModel model;
      model.setContext(staff, *staff->part()->staves());
      QVERIFY(!model.notes().isEmpty());

      Note* note = model.notes().front().note;
      QVERIFY(note && note->chord());
      const int notationStart = note->tick().ticks();
      const int notationDuration = note->playTicks();
      const int rootTicks = note->chord()->actualTicks().ticks();
      QVERIFY(rootTicks >= 4);

      const int desiredStart = notationStart + rootTicks / 4;
      const int desiredEnd = desiredStart + rootTicks / 2;
      const int sourceIndex = model.noteIndex(note);
      QVERIFY(sourceIndex >= 0);
      KeyEditorModel::NoteEdit edit { note, desiredStart, desiredEnd, note->pitch(),
                                      note->staffIdx(), note->voice(),
                                      model.notes()[sourceIndex].eventIndex };
      QVERIFY(model.applyPlaybackTimingEdits({ edit }));

      QCOMPARE(note->tick().ticks(), notationStart);
      QCOMPARE(note->playTicks(), notationDuration);
      const int index = model.noteIndex(note);
      QVERIFY(index >= 0);
      QCOMPARE(model.notes()[index].startTick, desiredStart);
      QCOMPARE(model.notes()[index].endTick, desiredEnd);

      testScore->undoRedo(true, nullptr);
      model.rebuild();
      QCOMPARE(note->tick().ticks(), notationStart);
      QCOMPARE(note->playTicks(), notationDuration);
      const int restoredIndex = model.noteIndex(note);
      QVERIFY(restoredIndex >= 0);
      QCOMPARE(model.notes()[restoredIndex].startTick, notationStart);

      delete testScore;
      }

void TestPianoRoll::tiedPlaybackResizeDoesNotCompound()
      {
      MasterScore* testScore = readScore(QStringLiteral("musicxml/io/importTie1_ref.mscx"));
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      QVERIFY(staff);
      KeyEditorModel model;
      model.setContext(staff, { staff });

      int tiedIndex = -1;
      for (int i = 0; i < model.notes().size(); ++i) {
            const KeyEditorModel::NoteBlock& block = model.notes()[i];
            if (block.note && block.note->tieFor()) {
                  tiedIndex = i;
                  break;
                  }
            }
      QVERIFY2(tiedIndex >= 0, "fixture must contain a rendered tied note");

      const KeyEditorModel::NoteBlock first = model.notes()[tiedIndex];
      const int firstTargetEnd = first.endTick + 120;
      KeyEditorModel::NoteEdit edit { first.note, first.startTick, firstTargetEnd,
                                      first.pitch, first.staffIdx, first.voice,
                                      first.eventIndex };
      QVERIFY(model.applyPlaybackTimingEdits({ edit }));
      int index = model.noteIndex(first.note, first.eventIndex);
      QVERIFY(index >= 0);
      QCOMPARE(model.notes()[index].endTick, firstTargetEnd);

      const KeyEditorModel::NoteBlock second = model.notes()[index];
      const int secondTargetEnd = second.endTick + 120;
      edit.startTick = second.startTick;
      edit.endTick = secondTargetEnd;
      QVERIFY(model.applyPlaybackTimingEdits({ edit }));
      index = model.noteIndex(first.note, first.eventIndex);
      QVERIFY(index >= 0);
      QCOMPARE(model.notes()[index].endTick, secondTargetEnd);

      const KeyEditorModel::NoteBlock shortened = model.notes()[index];
      const int shortTargetEnd = shortened.startTick
                               + qMax(1, shortened.note->chord()->actualTicks().ticks() / 2);
      QVERIFY2(shortTargetEnd < shortened.endTick,
               "fixture must allow shortening below the tied continuation");
      edit.startTick = shortened.startTick;
      edit.endTick = shortTargetEnd;
      QVERIFY(model.applyPlaybackTimingEdits({ edit }));
      index = model.noteIndex(first.note, first.eventIndex);
      QVERIFY(index >= 0);
      QCOMPARE(model.notes()[index].endTick, shortTargetEnd);
      QVERIFY(first.note->tieFor());
      QVERIFY(first.note->playEvents()[first.eventIndex].suppressTieTail());

      delete testScore;
      }

void TestPianoRoll::renderedEventsAreIndividuallyEditable()
      {
      MasterScore* testScore = readScore(
            QStringLiteral("testscript/scripts/palette_arpeggio_gliss_1.mscx"));
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      QVERIFY(staff);

      KeyEditorModel model;
      model.setContext(staff, { staff });
      QHash<Note*, QVector<int> > bySource;
      for (int i = 0; i < model.notes().size(); ++i) {
            const KeyEditorModel::NoteBlock& block = model.notes()[i];
            if (block.note && block.eventIndex >= 0)
                  bySource[block.note].append(i);
            }
      Note* source = nullptr;
      QVector<int> sourceBlocks;
      for (auto it = bySource.constBegin(); it != bySource.constEnd(); ++it) {
            if (it.value().size() > 1) {
                  source = it.key();
                  sourceBlocks = it.value();
                  break;
                  }
            }
      QVERIFY2(source, "fixture must contain a multi-event ornament or glissando");
      const int originalEventCount = source->playEvents().size();
      const int originalProjectionCount = model.notes().size();
      const int notationChordSize = source->chord()->notes().size();
      const int deletedEventIndex = model.notes()[sourceBlocks.front()].eventIndex;

      model.selectEvents({ sourceBlocks.front() }, KeyEditorModel::SelectionOperation::Replace);
      QCOMPARE(model.selectedEventIndexes().size(), 1);
      QVERIFY(model.deleteSelection());
      QCOMPARE(source->playEvents().size(), originalEventCount);
      QVERIFY(source->playEvents()[deletedEventIndex].suppressed());
      QCOMPARE(model.notes().size(), originalProjectionCount - 1);
      QCOMPARE(source->chord()->notes().size(), notationChordSize);

      testScore->undoRedo(true, nullptr);
      model.rebuild();
      QCOMPARE(source->playEvents().size(), originalEventCount);
      QCOMPARE(model.notes().size(), originalProjectionCount);

      delete testScore;
      }

void TestPianoRoll::selectionDoesNotInvalidatePlayback()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      QVERIFY(staff);

      KeyEditorModel model;
      model.setContext(staff, *staff->part()->staves());
      QVERIFY(!model.notes().isEmpty());

      // Reproduce the important precondition from the crash: the document has
      // an earlier edit on its undo stack, while its playback data is clean.
      Note* note = model.notes().front().note;
      QVERIFY(note);
      QHash<Note*, int> velocity;
      const int oldVelocity = model.effectiveVelocity(note);
      velocity.insert(note, oldVelocity < 127 ? oldVelocity + 1 : oldVelocity - 1);
      QVERIFY(model.setVelocities(velocity));
      QVERIFY(testScore->dirty());
      testScore->setPlaylistClean();
      QVERIFY(!testScore->playlistDirty());

      model.selectEvents({ 0 }, KeyEditorModel::SelectionOperation::Replace);
      QCOMPARE(model.selectedEventIndexes().size(), 1);
      QVERIFY(!testScore->playlistDirty());

      delete testScore;
      }

void TestPianoRoll::notationPitchRebasesPlaybackAdjustment()
      {
      MasterScore* testScore = readGrandStaffScore();
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      KeyEditorModel model;
      model.setContext(staff, *staff->part()->staves());
      QVERIFY(!model.notes().isEmpty());

      const KeyEditorModel::NoteBlock original = model.notes().front();
      Note* note = original.note;
      QVERIFY(note && note->chord());
      KeyEditorModel::NoteEdit playbackEdit { note, original.startTick,
            original.endTick, original.pitch + 2, original.staffIdx,
            original.voice, original.eventIndex };
      QVERIFY(model.applyPlaybackTimingEdits({ playbackEdit }));
      int index = model.noteIndex(note, original.eventIndex);
      QVERIFY(index >= 0);
      const int adjustedPitch = model.notes()[index].pitch;
      QCOMPARE(adjustedPitch, original.pitch + 2);

      const int notationPitch = note->pitch() + 1;
      testScore->startCmd();
      testScore->undoChangePitch(note, notationPitch,
            note->tpc1default(notationPitch), note->tpc2default(notationPitch));
      testScore->endCmd();
      testScore->createPlayEvents();
      model.rebuild();

      index = model.noteIndex(note, original.eventIndex);
      QVERIFY(index >= 0);
      QCOMPARE(model.notes()[index].pitch, adjustedPitch + 1);
      QCOMPARE(note->pitch(), notationPitch);
      delete testScore;
      }

void TestPianoRoll::removingOrnamentRegeneratesEditedPlayback()
      {
      MasterScore* testScore = readScore(
            QStringLiteral("testscript/scripts/palette_arpeggio_gliss_1.mscx"));
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      KeyEditorModel model;
      model.setContext(staff, { staff });

      Note* source = nullptr;
      QVector<int> sourceBlocks;
      QHash<Note*, QVector<int> > bySource;
      for (int i = 0; i < model.notes().size(); ++i)
            bySource[model.notes()[i].note].append(i);
      for (auto it = bySource.constBegin(); it != bySource.constEnd(); ++it) {
            if (it.key() && it.value().size() > 1) {
                  for (Spanner* spanner : it.key()->spannerFor()) {
                        if (spanner && spanner->isGlissando()) {
                              source = it.key();
                              sourceBlocks = it.value();
                              break;
                              }
                        }
                  }
            if (source)
                  break;
            }
      QVERIFY2(source, "fixture must contain a rendered glissando");
      const int editedBlockIndex = sourceBlocks.back();
      const KeyEditorModel::NoteBlock block = model.notes()[editedBlockIndex];
      KeyEditorModel::NoteEdit edit { source, block.startTick + 5,
            block.endTick + 5, block.pitch, block.staffIdx, block.voice,
            block.eventIndex };
      QVERIFY(model.applyPlaybackTimingEdits({ edit }));
      QVERIFY(source->playEvents()[block.eventIndex].playbackTracked());

      Spanner* glissando = nullptr;
      for (Spanner* spanner : source->spannerFor()) {
            if (spanner && spanner->isGlissando()) {
                  glissando = spanner;
                  break;
                  }
            }
      QVERIFY(glissando);
      testScore->startCmd();
      testScore->undoRemoveElement(glissando);
      testScore->endCmd();
      testScore->createPlayEvents();
      model.rebuild();

      QCOMPARE(source->playEvents().size(), 1);
      int sourceProjectionCount = 0;
      for (const KeyEditorModel::NoteBlock& projected : model.notes()) {
            if (projected.note == source)
                  ++sourceProjectionCount;
            }
      QCOMPARE(sourceProjectionCount, 1);
      delete testScore;
      }

QTEST_MAIN(TestPianoRoll)
#include "tst_pianoroll.moc"
