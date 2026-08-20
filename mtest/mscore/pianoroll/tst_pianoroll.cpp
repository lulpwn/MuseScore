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
      void projectionInvalidationKeepsViewport();
      void spaceRequestsPlaybackToggle();
      void controllerSelectorLivesInLane();
      void playbackTimingDoesNotChangeNotation();
      void graceNotesAreProjected();
      void arpeggioEventsAreProjected();
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

      model.setPlaybackTimingMode(true);
      const int desiredStart = notationStart + rootTicks / 4;
      const int desiredEnd = desiredStart + rootTicks / 2;
      KeyEditorModel::NoteEdit edit { note, desiredStart, desiredEnd, note->pitch(),
                                      note->staffIdx(), note->voice() };
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

void TestPianoRoll::graceNotesAreProjected()
      {
      MasterScore* testScore = readScore(QStringLiteral("libmscore/midi/testGraceBefore.mscx"));
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      QVERIFY(staff);

      KeyEditorModel model;
      model.setContext(staff, { staff });
      int graceCount = 0;
      for (const KeyEditorModel::NoteBlock& block : model.notes()) {
            if (!block.grace)
                  continue;
            ++graceCount;
            QVERIFY(block.note);
            QVERIFY(block.note->chord()->isGrace());
            QVERIFY(block.endTick > block.startTick);
            }
      QVERIFY(graceCount > 0);

      delete testScore;
      }

void TestPianoRoll::arpeggioEventsAreProjected()
      {
      MasterScore* testScore = readScore(
            QStringLiteral("testscript/scripts/palette_arpeggio_gliss_1.mscx"));
      QVERIFY(testScore);
      Staff* staff = testScore->staff(0);
      QVERIFY(staff);

      KeyEditorModel model;
      model.setContext(staff, { staff });
      bool foundOffsetEvent = false;
      for (const KeyEditorModel::NoteBlock& block : model.notes()) {
            if (block.eventIndex >= 0 && (block.ontime != 0
                || block.eventLength != NoteEvent::NOTE_LENGTH || block.pitchOffset != 0)) {
                  foundOffsetEvent = true;
                  break;
                  }
            }
      QVERIFY(foundOffsetEvent);

      delete testScore;
      }

QTEST_MAIN(TestPianoRoll)
#include "tst_pianoroll.moc"
