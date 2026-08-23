//=============================================================================
//  MuseScore
//  Key Editor view
//=============================================================================

#ifndef __KEYEDITOR_VIEW_H__
#define __KEYEDITOR_VIEW_H__

#include "keyeditormodel.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QRegion>
#include <QSet>
#include <QVector>

class QTimer;

namespace Ms {

class Note;
class Spanner;

class KeyEditorView : public QAbstractScrollArea
      {
      Q_OBJECT

   public:
      enum class EditTool : char { Select, Draw, Erase };
      enum class LaneMode : char { Velocity, Sustain, Tempo };
      enum class LaneTool : char { Pointer, Freehand, Line };

   private:
      enum class DragMode : char {
            None,
            Pan,
            Marquee,
            MoveNotes,
            ResizeLeft,
            ResizeRight,
            DrawNote,
            ResizeLane,
            VelocityHandle,
            VelocityFreehand,
            VelocityLine,
            VelocityBoxUniform,
            VelocityBoxLeft,
            VelocityBoxRight,
            PedalCreate,
            PedalMove,
            PedalStart,
            PedalEnd
            };
      enum class FocusDomain : char { Notes, Velocity, Sustain, Tempo };

      KeyEditorModel* _model { nullptr };
      EditTool _editTool { EditTool::Select };
      LaneMode _laneMode { LaneMode::Velocity };
      LaneTool _laneTool { LaneTool::Pointer };
      DragMode _dragMode { DragMode::None };

      bool _snapEnabled { true };
      int _gridTicks { 120 };
      int _editVoice { 0 };
      int _editStaffIdx { 0 };
      qreal _pixelsPerTick { 0.12 };
      int _keyHeight { 16 };
      int _controllerHeight { 240 };
      int _playbackTick { -1 };
      int _locatorTicks[3] { -1, -1, -1 };
      int _hoverPitch { -1 };
      int _cursorTick { 0 };
      int _auditionPitch { -1 };
      int _auditionStaffIdx { -1 };
      int _lastDragPreviewPitch { -1 };
      FocusDomain _focusDomain { FocusDomain::Notes };
      QTimer* _autoScrollTimer { nullptr };
      QWidget* _laneModeControl { nullptr };
      QWidget* _laneToolControl { nullptr };

      QPoint _pressPos;
      QPoint _lastPos;
      QPoint _panStart;
      int _panStartH { 0 };
      int _panStartV { 0 };
      int _pressTick { 0 };
      int _pressPitch { 60 };
      int _pressVelocity { 80 };
      bool _dragThresholdPassed { false };
      bool _duplicateDrag { false };
      Qt::KeyboardModifiers _pressModifiers;
      Note* _noteDragAnchor { nullptr };
      int _drawAnchorTick { 0 };
      int _drawAnchorPitch { 60 };

      QVector<KeyEditorModel::NoteEdit> _originalEdits;
      QVector<KeyEditorModel::NoteEdit> _previewEdits;
      QSet<int> _previewBlockIndexes;
      QRect _marquee;
      QPoint _marqueeContentAnchor;
      int _marqueeEndTick { 0 };
      int _marqueeEndPitch { 60 };
      KeyEditorModel::NoteEdit _drawPreview;

      QHash<int, int> _velocityOriginal;
      QHash<int, int> _velocityPreview;
      int _velocityAnchor { -1 };
      QRect _velocityTransformRect;

      Spanner* _selectedPedal { nullptr };
      int _pedalOriginalStart { 0 };
      int _pedalOriginalEnd { 1 };
      int _pedalPreviewStart { 0 };
      int _pedalPreviewEnd { 1 };
      int _pedalCreateAnchor { 0 };

      static constexpr int keyboardWidth = 78;
      static constexpr int rulerHeight = 30;
      static constexpr int laneSeparatorHeight = 6;
      static constexpr int controllerToolbarHeight = 34;
      static constexpr int minimumNoteAreaHeight = 120;

      QRect noteAreaRect() const;
      QRect rulerRect() const;
      QRect keyboardRect() const;
      QRect laneRect() const;
      QRect controllerContentRect() const;
      QRect laneSeparatorRect() const;
      QRect canvasRect() const;

      int tickToX(int tick) const;
      int xToTick(int x) const;
      int pitchToY(int pitch) const;
      int yToPitch(int y) const;
      int velocityToY(int velocity) const;
      int yToVelocity(int y) const;
      int snapTick(int tick, bool nearest = true) const;
      int snappedDragTick(int tick, bool nearest = true) const;
      int visibleStartTick() const;
      int visibleEndTick() const;
      int visibleHighPitch() const;
      int visibleLowPitch() const;
      QRectF noteRect(const KeyEditorModel::NoteBlock&) const;
      QRectF editRect(const KeyEditorModel::NoteEdit&) const;
      QRect pedalRect(const KeyEditorModel::PedalBlock&, bool preview = true) const;
      int velocityHandleX(const KeyEditorModel::NoteBlock&) const;
      QRect velocitySelectionRect() const;
      DragMode velocitySelectionHandleAt(const QPoint&) const;
      QRegion velocityGestureRegion(const QHash<int, int>&) const;
      QColor staffColor(int staffIdx) const;
      QColor noteColor(int staffIdx, int voice) const;
      bool isBlackKey(int pitch) const;
      int noteAtPoint(const QPoint&) const;

      void updateScrollBars();
      void updateDragRegion(const QRect& oldRegion = QRect());
      QRect previewRegion() const;
      void beginNoteDrag(int noteIndex, const QPoint&);
      void updateNoteDrag(const QPoint&, Qt::KeyboardModifiers);
      void finishNoteDrag();
      void beginMarquee(const QPoint&);
      void finishMarquee();
      void drawNoteAt(const QPoint&, bool commit);
      void selectNoteForClick(int noteIndex, Qt::KeyboardModifiers);
      void auditionPitchBriefly(int pitch, int staffIdx);
      void stopAudition();
      bool shouldAutoScrollForDrag(const QPoint&) const;
      bool autoScrollForDrag(const QPoint&);
      void refreshActiveDragAfterScroll();
      void updateCursorForPosition(const QPoint&);
      void updateLaneControlGeometry();
      void cancelActiveGesture();
      bool commitNoteEdits(const QVector<KeyEditorModel::NoteEdit>&, bool duplicate = false);
      bool nudgeSelection(int tickDelta, int pitchDelta, bool duplicate = false);
      bool quantizeSelection();

      int velocityHandleAt(const QPoint&) const;
      void beginVelocityGesture(const QPoint&);
      void updateVelocityGesture(const QPoint&);
      void finishVelocityGesture();
      void updateVelocityFreehand(const QPoint& from, const QPoint& to);
      void updateVelocityLine(const QPoint& from, const QPoint& to);
      Spanner* pedalAt(const QPoint&, DragMode* part = nullptr) const;
      void beginPedalGesture(const QPoint&);
      void updatePedalGesture(const QPoint&);
      void finishPedalGesture();

      void paintBackground(QPainter&, const QRect&);
      void paintGrid(QPainter&, const QRect&);
      void paintRuler(QPainter&, const QRect&);
      void paintKeyboard(QPainter&, const QRect&);
      void paintNotes(QPainter&, const QRect&);
      void paintControllerLane(QPainter&, const QRect&);
      void paintOverlays(QPainter&, const QRect&);
      void paintNote(QPainter&, const QRectF&, const KeyEditorModel::NoteBlock&, qreal alpha = 1.0);

      void contextMenuEvent(QContextMenuEvent*) override;
      void keyPressEvent(QKeyEvent*) override;
      void keyReleaseEvent(QKeyEvent*) override;
      void mousePressEvent(QMouseEvent*) override;
      void mouseMoveEvent(QMouseEvent*) override;
      void mouseReleaseEvent(QMouseEvent*) override;
      void mouseDoubleClickEvent(QMouseEvent*) override;
      void wheelEvent(QWheelEvent*) override;
      void paintEvent(QPaintEvent*) override;
      void resizeEvent(QResizeEvent*) override;
      void scrollContentsBy(int, int) override;
      void leaveEvent(QEvent*) override;
      void focusOutEvent(QFocusEvent*) override;
      void hideEvent(QHideEvent*) override;

   signals:
      void selectionChanged();
      void cursorChanged(int tick, int pitch);
      void seekRequested(int tick);
      void pitchPressed(int pitch, int staffIdx);
      void pitchReleased(int pitch);
      void notePreviewReleased();
      void zoomChanged(qreal horizontalZoom, int keyHeight);
      void editToolRequested(EditTool);
      void laneToolRequested(LaneTool);
      void laneModeRequested(LaneMode);
      void togglePlaybackRequested();
      void noteFocusRequested(Note*);
      void pianoRollSelectionStarted();

   public:
      explicit KeyEditorView(QWidget* parent = nullptr);

      void setModel(KeyEditorModel*);
      KeyEditorModel* model() const { return _model; }
      void setEditTool(EditTool);
      EditTool editTool() const { return _editTool; }
      void setLaneMode(LaneMode);
      LaneMode laneMode() const { return _laneMode; }
      void setLaneTool(LaneTool);
      LaneTool laneTool() const { return _laneTool; }
      void setSnapEnabled(bool);
      bool snapEnabled() const { return _snapEnabled; }
      void setGridTicks(int);
      int gridTicks() const { return _gridTicks; }
      void setEditVoice(int voice) { _editVoice = qBound(0, voice, 3); }
      void setEditStaffIdx(int index) { _editStaffIdx = index; }
      void setHorizontalZoom(qreal, int anchorX = -1);
      void setVerticalZoom(int, int anchorY = -1);
      qreal horizontalZoom() const { return _pixelsPerTick; }
      int verticalZoom() const { return _keyHeight; }
      void setControllerHeight(int);
      int controllerHeight() const { return _controllerHeight; }
      void setLaneControls(QWidget* modeControl, QWidget* toolControl);
      void setPlaybackTick(int);
      void setLocator(int index, int tick);
      void ensureTickVisible(int tick, bool center = false);
      void followPlaybackTick(int tick);
      void ensurePitchVisible(int pitch, bool center = false);
      int cursorTick() const { return _cursorTick; }
      Spanner* selectedPedal() const { return _selectedPedal; }
      bool interactionActive() const { return _dragMode != DragMode::None; }
      void refreshModel();
      };

} // namespace Ms

#endif
