//=============================================================================
//  MuseScore
//  Key Editor window
//=============================================================================

#ifndef __PIANOROLL_H__
#define __PIANOROLL_H__

#include "libmscore/mscoreview.h"
#include "libmscore/pos.h"
#include "libmscore/select.h"
#include "keyeditormodel.h"
#include "pianorolledittool.h"

#include <QList>
#include <QMainWindow>

class QAction;
class QButtonGroup;
class QComboBox;
class QLabel;
class QSpinBox;
class QToolButton;

namespace Ms {

class Element;
class KeyEditorView;
class Note;
struct Position;
class Score;
class Seq;
class Staff;
enum class POS : char;

class PianorollEditor final : public QMainWindow, public MuseScoreView
      {
      Q_OBJECT

      Staff* _staff { nullptr };
      QList<Staff*> _partStaves;
      KeyEditorModel* _model { nullptr };
      KeyEditorView* _view { nullptr };
      Pos _locators[3];
      bool _updateScheduled { false };
      int _auditionChannel { -1 };
      int _auditionPitch { -1 };
      bool _velocityDirty { false };
      bool _onTimeDirty { false };
      bool _eventLengthDirty { false };
      bool _selectionFromPianoRoll { false };

      QComboBox* _trackSelector { nullptr };
      QLabel* _editTargetLabel { nullptr };
      QComboBox* _editTargetSelector { nullptr };
      QToolButton* _snapButton { nullptr };
      QComboBox* _gridSelector { nullptr };
      QComboBox* _laneSelector { nullptr };
      QComboBox* _laneToolSelector { nullptr };
      QToolButton* _followButton { nullptr };
      QButtonGroup* _editToolGroup { nullptr };

      QSpinBox* _velocityField { nullptr };
      QSpinBox* _onTimeField { nullptr };
      QSpinBox* _eventLengthField { nullptr };
      QLabel* _selectionSummary { nullptr };
      QLabel* _cursorSummary { nullptr };

      QList<QAction*> _shortcutActions;

      void buildUi();
      void attachScore(Score*);
      void detachScore(bool removeViewer);
      void rebuildScopeSelectors(Staff*);
      void updateScope();
      void updateSelectionFields();
      void updateWindowTitle();
      void scheduleRebuild();
      void readSettings();
      Staff* selectedEditStaff() const;
      int selectedGridTicks() const;
      QVector<KeyEditorModel::NoteEdit> selectedEdits() const;
      bool applySelectedEdits(const QVector<KeyEditorModel::NoteEdit>&);
      void focusSelectedNoteInPianoRoll();
      void focusScoreOnNote(Note*);

   private slots:
      void doRebuild();
      void trackChanged(int);
      void editTargetChanged(int);
      void gridChanged(int);
      void laneChanged(int);
      void laneToolChanged(int);
      void editToolChanged(int);
      void selectionChanged();
      void cursorChanged(int tick, int pitch);
      void seekToTick(int tick);
      void pitchPressed(int pitch, int staffIdx);
      void pitchReleased(int pitch);
      void scorePositionChanged(POS, unsigned tick);
      void playlistChanged();
      void commitVelocity();
      void commitOnTime();
      void commitEventLength();

   public slots:
      void changeSelection(SelState);
      void handleAction(QAction*);

   public:
      explicit PianorollEditor(QWidget* parent = nullptr);
      ~PianorollEditor() override;

      void setScore(Score*) override;
      void setStaff(Staff*);
      void focusOnPosition(Position*);
      void heartBeat(Seq*);

      void setEditNoteLength(int);
      void setEditNoteVoice(int);
      void setEditNoteTool(PianoRollEditTool);
      void setEditNoteDots(int, QToolButton*);

      void dataChanged(const QRectF&) override;
      void updateAll() override;
      void removeScore() override;
      void changeEditElement(Element*) override;
      void onElementDestruction(Element*) override;
      QCursor cursor() const override;
      void setCursor(const QCursor&) override;
      Element* elementNear(QPointF) override;
      void drawBackground(QPainter*, const QRectF&) const override {}

      void writeSettings();
      const QRect geometry() const override { return QMainWindow::geometry(); }
      void zoom(int amount = 1, bool horizontal = true);
      };

} // namespace Ms

#endif
