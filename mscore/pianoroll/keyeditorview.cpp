//=============================================================================
//  MuseScore
//  Key Editor view
//=============================================================================

#include "keyeditorview.h"

#include "musescore.h"

#include "libmscore/measure.h"
#include "libmscore/note.h"
#include "libmscore/part.h"
#include "libmscore/score.h"
#include "libmscore/spanner.h"
#include "libmscore/staff.h"
#include "libmscore/tempo.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Ms {

extern MuseScore* mscore;

static QString pitchName(int pitch)
      {
      static const char* names[] = { "C", "C♯", "D", "E♭", "E", "F",
                                     "F♯", "G", "A♭", "A", "B♭", "B" };
      return QString::fromUtf8(names[(pitch % 12 + 12) % 12])
             + QString::number(pitch / 12 - 1);
      }

KeyEditorView::KeyEditorView(QWidget* parent)
   : QAbstractScrollArea(parent)
      {
      setObjectName(QStringLiteral("KeyEditorViewport"));
      setFrameShape(QFrame::NoFrame);
      setFocusPolicy(Qt::StrongFocus);
      setMouseTracking(true);
      viewport()->setMouseTracking(true);
      viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
      horizontalScrollBar()->setSingleStep(24);
      verticalScrollBar()->setSingleStep(_keyHeight * 3);

      _autoScrollTimer = new QTimer(this);
      _autoScrollTimer->setInterval(30);
      connect(_autoScrollTimer, &QTimer::timeout, this, [this]() {
            if (!autoScrollForDrag(_lastPos)) {
                  _autoScrollTimer->stop();
                  return;
                  }
            refreshActiveDragAfterScroll();
            });
      }

void KeyEditorView::setModel(KeyEditorModel* model)
      {
      if (_model == model)
            return;
      if (_model)
            disconnect(_model, nullptr, this, nullptr);
      _model = model;
      if (_model) {
            connect(_model, &KeyEditorModel::modelReset, this, [this]() {
                  _selectedPedal = nullptr;
                  _previewEdits.clear();
                  _previewBlockIndexes.clear();
                  _velocityPreview.clear();
                  updateScrollBars();
                  viewport()->update();
                  });
            connect(_model, &KeyEditorModel::selectionChanged, this, [this]() {
                  viewport()->update(noteAreaRect());
                  viewport()->update(laneRect());
                  emit selectionChanged();
                  });
            }
      updateScrollBars();
      viewport()->update();
      }

void KeyEditorView::refreshModel()
      {
      if (_model)
            _model->rebuild();
      else
            viewport()->update();
      }

QRect KeyEditorView::noteAreaRect() const
      {
      const int separatorTop = qMax(rulerHeight + minimumNoteAreaHeight,
                                    viewport()->height() - _controllerHeight - laneSeparatorHeight);
      return QRect(keyboardWidth, rulerHeight,
                   qMax(0, viewport()->width() - keyboardWidth),
                   qMax(0, separatorTop - rulerHeight));
      }

QRect KeyEditorView::rulerRect() const
      {
      return QRect(keyboardWidth, 0, qMax(0, viewport()->width() - keyboardWidth), rulerHeight);
      }

QRect KeyEditorView::keyboardRect() const
      {
      const QRect notes = noteAreaRect();
      return QRect(0, notes.top(), keyboardWidth, notes.height());
      }

QRect KeyEditorView::laneSeparatorRect() const
      {
      const QRect notes = noteAreaRect();
      return QRect(0, notes.bottom() + 1, viewport()->width(), laneSeparatorHeight);
      }

QRect KeyEditorView::laneRect() const
      {
      const int top = laneSeparatorRect().bottom() + 1;
      return QRect(keyboardWidth, top, qMax(0, viewport()->width() - keyboardWidth),
                   qMax(0, viewport()->height() - top));
      }

QRect KeyEditorView::controllerContentRect() const
      {
      return laneRect().adjusted(0, controllerToolbarHeight, 0, 0);
      }

QRect KeyEditorView::canvasRect() const
      {
      return noteAreaRect();
      }

int KeyEditorView::tickToX(int tick) const
      {
      return keyboardWidth + qRound(tick * _pixelsPerTick) - horizontalScrollBar()->value();
      }

int KeyEditorView::xToTick(int x) const
      {
      return qMax(0, qRound((x - keyboardWidth + horizontalScrollBar()->value()) / _pixelsPerTick));
      }

int KeyEditorView::pitchToY(int pitch) const
      {
      return rulerHeight + (127 - pitch) * _keyHeight - verticalScrollBar()->value();
      }

int KeyEditorView::yToPitch(int y) const
      {
      return qBound(0, 127 - ((y - rulerHeight + verticalScrollBar()->value()) / qMax(1, _keyHeight)), 127);
      }

int KeyEditorView::velocityToY(int velocity) const
      {
      const QRect lane = controllerContentRect().adjusted(0, 0, 0, -10);
      const qreal ratio = (qBound(1, velocity, 127) - 1) / 126.0;
      return lane.bottom() - qRound(ratio * qMax(1, lane.height() - 1));
      }

int KeyEditorView::yToVelocity(int y) const
      {
      const QRect lane = controllerContentRect().adjusted(0, 0, 0, -10);
      const qreal ratio = (lane.bottom() - y) / qreal(qMax(1, lane.height() - 1));
      return qBound(1, qRound(ratio * 126.0 + 1.0), 127);
      }

int KeyEditorView::snapTick(int tick, bool nearest) const
      {
      tick = qMax(0, tick);
      if (!_snapEnabled || _gridTicks <= 1)
            return tick;
      if (!nearest)
            return (tick / _gridTicks) * _gridTicks;
      return qMax(0, qRound(tick / qreal(_gridTicks)) * _gridTicks);
      }

int KeyEditorView::snappedDragTick(int tick, bool nearest) const
      {
      return (_pressModifiers & Qt::ShiftModifier) ? qMax(0, tick) : snapTick(tick, nearest);
      }

int KeyEditorView::visibleStartTick() const
      {
      return xToTick(noteAreaRect().left());
      }

int KeyEditorView::visibleEndTick() const
      {
      return xToTick(noteAreaRect().right());
      }

int KeyEditorView::visibleHighPitch() const
      {
      return yToPitch(noteAreaRect().top());
      }

int KeyEditorView::visibleLowPitch() const
      {
      return yToPitch(noteAreaRect().bottom());
      }

QRectF KeyEditorView::noteRect(const KeyEditorModel::NoteBlock& block) const
      {
      const qreal x1 = tickToX(block.startTick);
      const qreal x2 = tickToX(block.endTick);
      qreal y = pitchToY(block.pitch) + 1;
      qreal height = qMax(3, _keyHeight - 2);
      if (block.overlap && _model && _model->visibleStaves().size() > 1) {
            const int count = _model->visibleStaves().size();
            int row = 0;
            for (int i = 0; i < count; ++i) {
                  if (_model->visibleStaves()[i]->idx() == block.staffIdx) {
                        row = i;
                        break;
                        }
                  }
            height = qMax<qreal>(3.0, (_keyHeight - 2.0) / count);
            y += row * height;
            }
      if (block.grace) {
            const qreal graceHeight = qMax<qreal>(3.0, height * 0.72);
            y += (height - graceHeight) * 0.5;
            height = graceHeight;
            }
      return QRectF(x1, y, qMax<qreal>(2.0, x2 - x1), height);
      }

QRectF KeyEditorView::editRect(const KeyEditorModel::NoteEdit& edit) const
      {
      const qreal x1 = tickToX(edit.startTick);
      const qreal x2 = tickToX(edit.endTick);
      qreal y = pitchToY(edit.pitch) + 1;
      qreal height = qMax(3, _keyHeight - 2);
      const int index = _model ? _model->noteIndex(edit.source, edit.eventIndex) : -1;
      if (index >= 0 && _model->notes()[index].overlap && _model->visibleStaves().size() > 1) {
            const int count = _model->visibleStaves().size();
            int row = 0;
            for (int i = 0; i < count; ++i) {
                  if (_model->visibleStaves()[i]->idx() == edit.staffIdx) {
                        row = i;
                        break;
                        }
                  }
            height = qMax<qreal>(3.0, (_keyHeight - 2.0) / count);
            y += row * height;
            }
      if (index >= 0 && _model->notes()[index].grace) {
            const qreal graceHeight = qMax<qreal>(3.0, height * 0.72);
            y += (height - graceHeight) * 0.5;
            height = graceHeight;
            }
      return QRectF(x1, y, qMax<qreal>(2.0, x2 - x1), height);
      }

QColor KeyEditorView::staffColor(int staffIdx) const
      {
      static const QColor colors[] = {
            QColor(74, 144, 226), QColor(238, 139, 61), QColor(68, 170, 117),
            QColor(160, 112, 210), QColor(219, 90, 127), QColor(52, 174, 187)
            };
      int partIndex = 0;
      if (_model) {
            const QList<Staff*>& staves = _model->visibleStaves();
            if (_model->editStaff() && _model->editStaff()->part()) {
                  const QList<Staff*>* partStaves = _model->editStaff()->part()->staves();
                  for (int i = 0; partStaves && i < partStaves->size(); ++i) {
                        if (partStaves->at(i)->idx() == staffIdx) {
                              partIndex = i;
                              break;
                              }
                        }
                  }
            else {
                  for (int i = 0; i < staves.size(); ++i) {
                        if (staves[i] && staves[i]->idx() == staffIdx) {
                              partIndex = i;
                              break;
                              }
                        }
                  }
            }
      return colors[partIndex % 6];
      }

QColor KeyEditorView::noteColor(int staffIdx, int voice) const
      {
      const QColor staff = staffColor(staffIdx);
      voice = qBound(0, voice, 3);
      if (voice == 0)
            return staff;

      static const QColor voiceColors[] = {
            QColor(74, 144, 226), QColor(229, 194, 59),
            QColor(48, 183, 193), QColor(184, 91, 211)
            };
      const QColor voiceBase = voiceColors[voice];
      constexpr int voiceWeight = 82;
      constexpr int staffWeight = 100 - voiceWeight;
      return QColor((voiceBase.red() * voiceWeight + staff.red() * staffWeight) / 100,
                    (voiceBase.green() * voiceWeight + staff.green() * staffWeight) / 100,
                    (voiceBase.blue() * voiceWeight + staff.blue() * staffWeight) / 100);
      }

bool KeyEditorView::isBlackKey(int pitch) const
      {
      const int pc = (pitch % 12 + 12) % 12;
      return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
      }

int KeyEditorView::noteAtPoint(const QPoint& point) const
      {
      if (!_model || !noteAreaRect().contains(point))
            return -1;
      const int tick = xToTick(point.x());
      const int pitch = yToPitch(point.y());
      const QVector<int> hits = _model->notesInRange(tick, tick, pitch, pitch);
      for (auto it = hits.crbegin(); it != hits.crend(); ++it) {
            if (noteRect(_model->notes()[*it]).adjusted(-1, -1, 1, 1).contains(point))
                  return *it;
            }
      return -1;
      }

QRect KeyEditorView::pedalRect(const KeyEditorModel::PedalBlock& block, bool preview) const
      {
      int start = block.startTick;
      int end = block.endTick;
      if (preview && block.spanner == _selectedPedal
          && (_dragMode == DragMode::PedalMove || _dragMode == DragMode::PedalStart
              || _dragMode == DragMode::PedalEnd)) {
            start = _pedalPreviewStart;
            end = _pedalPreviewEnd;
            }
      const QRect lane = controllerContentRect().adjusted(0, 5, 0, -10);
      int staffRow = 0;
      int rowCount = 1;
      if (_model) {
            rowCount = qMax(1, _model->visibleStaves().size());
            for (int i = 0; i < _model->visibleStaves().size(); ++i) {
                  if (_model->visibleStaves()[i]->idx() == block.staffIdx) {
                        staffRow = i;
                        break;
                        }
                  }
            }
      const int rowHeight = qMax(18, lane.height() / rowCount);
      const int y = lane.top() + staffRow * rowHeight + 4;
      return QRect(tickToX(start), y, qMax(3, tickToX(end) - tickToX(start)), qMax(12, rowHeight - 8));
      }

int KeyEditorView::velocityHandleX(const KeyEditorModel::NoteBlock& block) const
      {
      int x = tickToX(block.startTick);
      if (!_model || _model->visibleStaves().size() <= 1)
            return x;
      const int count = _model->visibleStaves().size();
      int row = 0;
      for (int i = 0; i < count; ++i) {
            if (_model->visibleStaves()[i]->idx() == block.staffIdx) {
                  row = i;
                  break;
                  }
            }
      return x + qRound((row - (count - 1) / 2.0) * 5.0);
      }

QRect KeyEditorView::velocitySelectionRect() const
      {
      if (!_model || _laneMode != LaneMode::Velocity)
            return QRect();
      const QVector<int> selected = _model->selectedEventIndexes();
      if (selected.size() < 2)
            return QRect();
      int left = INT_MAX;
      int right = INT_MIN;
      int top = INT_MAX;
      for (int index : selected) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            const int x = velocityHandleX(block);
            const int value = _velocityPreview.value(index, block.velocity);
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, velocityToY(value));
            }
      if (left == INT_MAX)
            return QRect();
      if (right <= left) {
            left -= 12;
            right += 12;
            }
      const QRect graph = controllerContentRect().adjusted(0, 0, 0, -10);
      top = qMax(graph.top() + 4, top - 9);
      return QRect(QPoint(left, top), QPoint(right, velocityToY(1)));
      }

KeyEditorView::DragMode KeyEditorView::velocitySelectionHandleAt(const QPoint& point) const
      {
      const QRect box = velocitySelectionRect();
      if (box.isNull())
            return DragMode::None;
      constexpr int radius = 7;
      const QPoint handles[] = {
            box.topLeft(), QPoint(box.center().x(), box.top()), box.topRight()
            };
      const DragMode modes[] = {
            DragMode::VelocityBoxLeft, DragMode::VelocityBoxUniform,
            DragMode::VelocityBoxRight
            };
      for (int i = 0; i < 3; ++i) {
            if (QRect(handles[i] - QPoint(radius, radius), QSize(radius * 2 + 1,
                                                                 radius * 2 + 1)).contains(point))
                  return modes[i];
            }
      return DragMode::None;
      }

QRegion KeyEditorView::velocityGestureRegion(const QHash<int, int>& values) const
      {
      QRegion region;
      if (!_model)
            return region;
      const int baseline = velocityToY(1);
      for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const int index = it.key();
            if (index < 0)
                  continue;
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            const int x = velocityHandleX(block);
            const int y = velocityToY(it.value());
            region += QRect(x - 6, qMin(y, baseline) - 6, 13,
                            qAbs(baseline - y) + 13);
            }
      return region.intersected(laneRect());
      }

void KeyEditorView::updateScrollBars()
      {
      const int endTick = _model ? _model->scoreEndTick() : 0;
      const int extraTicks = qMax(DIVISION * 4, _gridTicks * 8);
      const int contentWidth = qRound((endTick + extraTicks) * _pixelsPerTick);
      horizontalScrollBar()->setRange(0, qMax(0, contentWidth - noteAreaRect().width()));
      horizontalScrollBar()->setPageStep(qMax(1, noteAreaRect().width()));
      horizontalScrollBar()->setSingleStep(qMax(8, qRound(_gridTicks * _pixelsPerTick)));

      const int contentHeight = 128 * _keyHeight;
      verticalScrollBar()->setRange(0, qMax(0, contentHeight - noteAreaRect().height()));
      verticalScrollBar()->setPageStep(qMax(1, noteAreaRect().height()));
      verticalScrollBar()->setSingleStep(qMax(1, _keyHeight * 3));
      }

void KeyEditorView::setEditTool(EditTool tool)
      {
      _editTool = tool;
      viewport()->setCursor(tool == EditTool::Draw ? Qt::CrossCursor
                            : tool == EditTool::Erase ? Qt::ForbiddenCursor
                                                     : Qt::ArrowCursor);
      }

void KeyEditorView::setLaneMode(LaneMode mode)
      {
      if (_laneMode == mode)
            return;
      _laneMode = mode;
      _selectedPedal = nullptr;
      _velocityPreview.clear();
      viewport()->update(laneRect().united(QRect(0, laneRect().top(), keyboardWidth, laneRect().height())));
      }

void KeyEditorView::setLaneTool(LaneTool tool)
      {
      _laneTool = tool;
      }

void KeyEditorView::setSnapEnabled(bool enabled)
      {
      _snapEnabled = enabled;
      viewport()->update(noteAreaRect());
      }

void KeyEditorView::setGridTicks(int ticks)
      {
      _gridTicks = qMax(1, ticks);
      updateScrollBars();
      viewport()->update(noteAreaRect().united(rulerRect()));
      }

void KeyEditorView::setHorizontalZoom(qreal zoom, int anchorX)
      {
      zoom = qBound<qreal>(0.018, zoom, 1.6);
      if (qFuzzyCompare(zoom, _pixelsPerTick))
            return;
      if (anchorX < keyboardWidth)
            anchorX = keyboardWidth + noteAreaRect().width() / 2;
      const qreal anchorTick = (anchorX - keyboardWidth + horizontalScrollBar()->value()) / _pixelsPerTick;
      _pixelsPerTick = zoom;
      updateScrollBars();
      horizontalScrollBar()->setValue(qRound(anchorTick * _pixelsPerTick - (anchorX - keyboardWidth)));
      viewport()->update();
      emit zoomChanged(_pixelsPerTick, _keyHeight);
      }

void KeyEditorView::setVerticalZoom(int height, int anchorY)
      {
      height = qBound(8, height, 34);
      if (height == _keyHeight)
            return;
      if (anchorY < rulerHeight || anchorY > noteAreaRect().bottom())
            anchorY = noteAreaRect().center().y();
      const qreal anchorRow = (anchorY - rulerHeight + verticalScrollBar()->value()) / qreal(_keyHeight);
      _keyHeight = height;
      updateScrollBars();
      verticalScrollBar()->setValue(qRound(anchorRow * _keyHeight - (anchorY - rulerHeight)));
      viewport()->update();
      emit zoomChanged(_pixelsPerTick, _keyHeight);
      }

void KeyEditorView::setControllerHeight(int height)
      {
      const int maxHeight = qMax(80, viewport()->height() - rulerHeight - minimumNoteAreaHeight - laneSeparatorHeight);
      _controllerHeight = maxHeight <= 80 ? qMax(80, height) : qBound(80, height, maxHeight);
      updateScrollBars();
      updateLaneControlGeometry();
      viewport()->update();
      }

void KeyEditorView::setLaneControls(QWidget* modeControl, QWidget* toolControl)
      {
      _laneModeControl = modeControl;
      _laneToolControl = toolControl;
      for (QWidget* control : { _laneModeControl, _laneToolControl }) {
            if (!control)
                  continue;
            control->setParent(viewport());
            control->show();
            control->raise();
            }
      updateLaneControlGeometry();
      }

void KeyEditorView::updateLaneControlGeometry()
      {
      const QRect toolbar(0, laneRect().top(), viewport()->width(), controllerToolbarHeight);
      const int height = qMax(22, toolbar.height() - 8);
      int x = 7;
      if (_laneModeControl) {
            _laneModeControl->setGeometry(x, toolbar.top() + 4, 146, height);
            _laneModeControl->raise();
            x += 152;
            }
      if (_laneToolControl) {
            _laneToolControl->setGeometry(x, toolbar.top() + 4, 148, height);
            _laneToolControl->raise();
            }
      }

void KeyEditorView::setPlaybackTick(int tick)
      {
      if (_playbackTick == tick)
            return;
      const int oldX = _playbackTick >= 0 ? tickToX(_playbackTick) : -100;
      _playbackTick = tick;
      const int newX = _playbackTick >= 0 ? tickToX(_playbackTick) : -100;
      viewport()->update(QRect(oldX - 8, 0, 17, viewport()->height()));
      viewport()->update(QRect(newX - 8, 0, 17, viewport()->height()));
      }

void KeyEditorView::setLocator(int index, int tick)
      {
      if (index < 0 || index > 2)
            return;
      const int oldX = _locatorTicks[index] >= 0 ? tickToX(_locatorTicks[index]) : -100;
      _locatorTicks[index] = tick;
      const int newX = tick >= 0 ? tickToX(tick) : -100;
      viewport()->update(QRect(oldX - 8, 0, 17, viewport()->height()));
      viewport()->update(QRect(newX - 8, 0, 17, viewport()->height()));
      }

void KeyEditorView::ensureTickVisible(int tick, bool center)
      {
      const int x = tickToX(tick);
      const QRect area = noteAreaRect();
      if (center)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x - area.center().x());
      else if (x < area.left() + 24)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x - area.left() - 24);
      else if (x > area.right() - 24)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x - area.right() + 24);
      }

void KeyEditorView::followPlaybackTick(int tick)
      {
      const QRect area = noteAreaRect();
      if (area.width() <= 0)
            return;
      const int x = tickToX(tick);
      const int jumpAt = area.left() + qRound(area.width() * 0.78);
      if (x >= area.left() && x < jumpAt)
            return;
      const int landing = area.left() + qRound(area.width() * 0.16);
      horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x - landing);
      }

void KeyEditorView::ensurePitchVisible(int pitch, bool center)
      {
      pitch = qBound(0, pitch, 127);
      const int y = pitchToY(pitch) + _keyHeight / 2;
      const QRect area = noteAreaRect();
      if (center)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + y - area.center().y());
      else if (y < area.top() + _keyHeight)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + y - area.top() - _keyHeight);
      else if (y > area.bottom() - _keyHeight)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + y - area.bottom() + _keyHeight);
      }

void KeyEditorView::paintBackground(QPainter& painter, const QRect& dirty)
      {
      const QPalette palette = QApplication::palette();
      painter.fillRect(dirty, palette.color(QPalette::Window));
      painter.fillRect(noteAreaRect().intersected(dirty), palette.color(QPalette::Base));
      painter.fillRect(rulerRect().intersected(dirty), palette.color(QPalette::Button));
      painter.fillRect(keyboardRect().intersected(dirty), palette.color(QPalette::Button));
      painter.fillRect(laneRect().intersected(dirty), palette.color(QPalette::AlternateBase));
      painter.fillRect(QRect(0, laneRect().top(), keyboardWidth, laneRect().height()).intersected(dirty),
                       palette.color(QPalette::Button));
      painter.fillRect(laneSeparatorRect().intersected(dirty), palette.color(QPalette::Mid));
      painter.fillRect(QRect(0, 0, keyboardWidth, rulerHeight).intersected(dirty),
                       palette.color(QPalette::Button));
      }

void KeyEditorView::paintGrid(QPainter& painter, const QRect& dirty)
      {
      const QRect area = noteAreaRect().intersected(dirty);
      if (area.isEmpty())
            return;
      painter.save();
      painter.setClipRect(area);
      const QPalette palette = QApplication::palette();
      QColor blackRow = palette.color(QPalette::AlternateBase);
      blackRow.setAlpha(105);
      QColor line = palette.color(QPalette::Mid);
      line.setAlpha(90);
      const int high = yToPitch(area.top());
      const int low = yToPitch(area.bottom());
      for (int pitch = low; pitch <= high; ++pitch) {
            const QRect row(noteAreaRect().left(), pitchToY(pitch), noteAreaRect().width(), _keyHeight);
            if (isBlackKey(pitch))
                  painter.fillRect(row, blackRow);
            painter.setPen(QPen(line, 1));
            painter.drawLine(row.bottomLeft(), row.bottomRight());
            }

      const int startTick = xToTick(area.left());
      const int endTick = xToTick(area.right());
      int gridStep = qMax(1, _gridTicks);
      while (gridStep * _pixelsPerTick < 8.0)
            gridStep *= 2;
      const int firstGrid = (startTick / gridStep) * gridStep;
      QColor gridColor = palette.color(QPalette::Mid);
      gridColor.setAlpha(75);
      painter.setPen(QPen(gridColor, 1));
      for (int tick = firstGrid; tick <= endTick + gridStep; tick += gridStep) {
            const int x = tickToX(tick);
            painter.drawLine(x, area.top(), x, area.bottom());
            }

      if (_model && _model->score()) {
            Measure* measure = _model->score()->tick2measure(Fraction::fromTicks(startTick));
            for (; measure && measure->tick().ticks() <= endTick; measure = measure->nextMeasure()) {
                  const int x = tickToX(measure->tick().ticks());
                  QColor barColor = palette.color(QPalette::Text);
                  barColor.setAlpha(105);
                  painter.setPen(QPen(barColor, 1.5));
                  painter.drawLine(x, area.top(), x, area.bottom());
                  const int beats = qMax(1, measure->timesig().numerator());
                  const int beatTicks = qMax(1, measure->ticks().ticks() / beats);
                  QColor beatColor = palette.color(QPalette::Mid);
                  beatColor.setAlpha(110);
                  painter.setPen(QPen(beatColor, 1));
                  for (int beat = 1; beat < beats; ++beat) {
                        const int beatX = tickToX(measure->tick().ticks() + beat * beatTicks);
                        painter.drawLine(beatX, area.top(), beatX, area.bottom());
                        }
                  }
            }
      painter.restore();
      }

void KeyEditorView::paintRuler(QPainter& painter, const QRect& dirty)
      {
      const QRect area = rulerRect().intersected(dirty);
      if (area.isEmpty())
            return;
      painter.save();
      painter.setClipRect(area);
      const QPalette palette = QApplication::palette();
      painter.setFont(QFont(painter.font().family(), 8));
      if (_model && _model->score()) {
            const int startTick = visibleStartTick();
            const int endTick = visibleEndTick();
            Measure* measure = _model->score()->tick2measure(Fraction::fromTicks(startTick));
            for (; measure && measure->tick().ticks() <= endTick; measure = measure->nextMeasure()) {
                  const int x = tickToX(measure->tick().ticks());
                  painter.setPen(palette.color(QPalette::Mid));
                  painter.drawLine(x, area.top() + 12, x, area.bottom());
                  painter.setPen(palette.color(QPalette::Text));
                  painter.drawText(QRect(x + 5, area.top(), 60, area.height() - 2),
                                   Qt::AlignLeft | Qt::AlignVCenter,
                                   QString::number(measure->no() + 1));
                  }
            }
      painter.setPen(palette.color(QPalette::Mid));
      painter.drawLine(area.bottomLeft(), area.bottomRight());
      painter.restore();
      }

void KeyEditorView::paintKeyboard(QPainter& painter, const QRect& dirty)
      {
      const QRect area = keyboardRect().intersected(dirty);
      if (area.isEmpty())
            return;
      painter.save();
      painter.setClipRect(area);
      const QPalette palette = QApplication::palette();
      painter.setFont(QFont(painter.font().family(), qMax(7, qMin(9, _keyHeight - 3))));
      painter.fillRect(keyboardRect(), QColor(238, 239, 241));

      // White keys occupy their natural regions; black keys are shorter
      // overlays centred between them.  Treating every semitone as a full key
      // made the black keys form long, stacked bars down the keyboard.
      const int low = qMax(0, visibleLowPitch() - 2);
      const int high = qMin(127, visibleHighPitch() + 2);
      auto whiteStepAbove = [](int pitchClass) {
            static const int steps[] = { 2, 0, 2, 0, 1, 2, 0, 2, 0, 2, 0, 1 };
            return steps[pitchClass];
            };
      auto whiteStepBelow = [](int pitchClass) {
            static const int steps[] = { 1, 0, 2, 0, 2, 1, 0, 2, 0, 2, 0, 2 };
            return steps[pitchClass];
            };
      for (int pitch = low; pitch <= high; ++pitch) {
            if (isBlackKey(pitch))
                  continue;
            const int pitchClass = (pitch % 12 + 12) % 12;
            const int center = pitchToY(pitch) + _keyHeight / 2;
            const int above = pitchToY(qMin(127, pitch + whiteStepAbove(pitchClass))) + _keyHeight / 2;
            const int below = pitchToY(qMax(0, pitch - whiteStepBelow(pitchClass))) + _keyHeight / 2;
            QRect key(0, (center + above) / 2, keyboardWidth,
                      qMax(1, (center + below) / 2 - (center + above) / 2));
            const QColor fill = pitch == _hoverPitch
                              ? staffColor(_editStaffIdx).lighter(165)
                              : QColor(238, 239, 241);
            painter.fillRect(key, fill);
            painter.setPen(QColor(145, 147, 151));
            painter.drawRect(key.adjusted(0, 0, -1, -1));
            if (pitch % 12 == 0 && _keyHeight >= 12) {
                  painter.setPen(QColor(45, 45, 48));
                  painter.drawText(key.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                                   pitchName(pitch));
                  }
            }
      for (int pitch = low; pitch <= high; ++pitch) {
            if (!isBlackKey(pitch))
                  continue;
            const int keyHeight = qMax(4, qRound(_keyHeight * 0.62));
            const int center = pitchToY(pitch) + _keyHeight / 2;
            QRect key(0, center - keyHeight / 2, keyboardWidth - 21, keyHeight);
            const QColor fill = pitch == _hoverPitch
                              ? staffColor(_editStaffIdx).lighter(125)
                              : QColor(48, 50, 54);
            painter.setBrush(fill);
            painter.setPen(QColor(20, 20, 20));
            painter.drawRoundedRect(key, 1.5, 1.5);
            }
      painter.setPen(palette.color(QPalette::Mid));
      painter.drawLine(area.topRight(), area.bottomRight());
      painter.restore();
      }

void KeyEditorView::paintNote(QPainter& painter, const QRectF& rect,
                              const KeyEditorModel::NoteBlock& block, qreal alpha)
      {
      QColor fill = noteColor(block.staffIdx, block.voice);
      fill.setAlphaF(qBound<qreal>(0.0, alpha, 1.0));
      const bool selected = block.note && _model
                         && _model->eventSelected(block.note, block.eventIndex);
      const QColor originalColor = fill;
      QColor outline = selected ? originalColor : fill.darker(180);
      QColor selectedFill(
            qRound(originalColor.red() * 0.42 + 255 * 0.58),
            qRound(originalColor.green() * 0.42 + 255 * 0.58),
            qRound(originalColor.blue() * 0.42 + 255 * 0.58),
            qRound(255 * alpha));
      painter.setBrush(selected ? selectedFill : fill);
      painter.setPen(QPen(outline, selected ? 1.6 : 1.0));
      painter.drawRoundedRect(rect, 2.5, 2.5);

      if (block.overlap) {
            painter.setPen(QPen(outline, 1.3));
            for (qreal x = rect.left() + 7; x < rect.right(); x += 8)
                  painter.drawLine(QPointF(x, rect.top() + 1),
                                   QPointF(qMin(x + 6, rect.right() - 1), rect.bottom() - 1));
            }

      if (rect.width() > 35 && rect.height() >= 12) {
            painter.setPen(selected ? QColor(25, 25, 27, 235)
                                    : QColor(255, 255, 255, 235));
            QString label = block.grace ? QStringLiteral("g  ") + pitchName(block.pitch)
                                        : pitchName(block.pitch);
            if (block.voice > 0)
                  label += QStringLiteral("  V%1").arg(block.voice + 1);
            painter.drawText(rect.adjusted(5, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
            }
      }

void KeyEditorView::paintNotes(QPainter& painter, const QRect& dirty)
      {
      const QRect area = noteAreaRect().intersected(dirty);
      if (area.isEmpty() || !_model)
            return;
      painter.save();
      painter.setClipRect(area);
      const QVector<int> visible = _model->notesInRange(xToTick(area.left()), xToTick(area.right()),
                                                        yToPitch(area.bottom()), yToPitch(area.top()));
      for (int index : visible) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            if (!_duplicateDrag && _previewBlockIndexes.contains(index))
                  continue;
            const QRectF rect = noteRect(block);
            if (rect.intersects(area))
                  paintNote(painter, rect, block);
            }
      for (const KeyEditorModel::NoteEdit& edit : _previewEdits) {
            const int sourceIndex = _model->noteIndex(edit.source, edit.eventIndex);
            if (sourceIndex < 0)
                  continue;
            KeyEditorModel::NoteBlock preview = _model->notes()[sourceIndex];
            preview.startTick = edit.startTick;
            preview.endTick = edit.endTick;
            preview.pitch = edit.pitch;
            preview.staffIdx = edit.staffIdx;
            preview.voice = edit.voice;
            paintNote(painter, editRect(edit), preview, 0.78);
            }
      if (_dragMode == DragMode::DrawNote && _dragThresholdPassed) {
            KeyEditorModel::NoteBlock preview;
            preview.startTick = _drawPreview.startTick;
            preview.endTick = _drawPreview.endTick;
            preview.pitch = _drawPreview.pitch;
            preview.staffIdx = _drawPreview.staffIdx;
            preview.voice = _drawPreview.voice;
            paintNote(painter, editRect(_drawPreview), preview, 0.72);
            }
      painter.restore();
      }

void KeyEditorView::paintControllerLane(QPainter& painter, const QRect& dirty)
      {
      const QRect lane = controllerContentRect().intersected(dirty);
      const QRect controllerToolbar(0, laneRect().top(), viewport()->width(),
                                    controllerToolbarHeight);
      const QRect header = QRect(0, controllerContentRect().top(), keyboardWidth,
                                 controllerContentRect().height()).intersected(dirty);
      if (lane.isEmpty() && header.isEmpty() && !controllerToolbar.intersects(dirty))
            return;
      const QPalette palette = QApplication::palette();
      if (controllerToolbar.intersects(dirty)) {
            painter.fillRect(controllerToolbar.intersected(dirty), palette.color(QPalette::Button));
            }
      if (!header.isEmpty()) {
            painter.save();
            painter.setClipRect(header);
            painter.setPen(palette.color(QPalette::Text));
            painter.setFont(QFont(painter.font().family(), 8, QFont::DemiBold));
            if (_laneMode == LaneMode::Velocity) {
                  for (int guide : { 32, 64, 96 }) {
                        const int y = velocityToY(guide);
                        painter.drawText(QRect(3, y - 7, keyboardWidth - 9, 14),
                                         Qt::AlignRight | Qt::AlignVCenter,
                                         QString::number(guide));
                        }
                  }
            else if (_laneMode == LaneMode::Tempo) {
                  const QRect graph = controllerContentRect().adjusted(0, 8, 0, -10);
                  for (int bpm : { 60, 120, 180, 240 }) {
                        const qreal ratio = (bpm - 20.0) / 280.0;
                        const int y = graph.bottom() - qRound(ratio * qMax(1, graph.height()));
                        painter.drawText(QRect(3, y - 7, keyboardWidth - 9, 14),
                                         Qt::AlignRight | Qt::AlignVCenter,
                                         QString::number(bpm));
                        }
                  }
            else
                  painter.drawText(header.adjusted(8, 8, -5, -5),
                                   Qt::AlignTop | Qt::AlignLeft, tr("SUSTAIN\nCC64"));
            painter.restore();
            }
      if (lane.isEmpty() || !_model)
            return;

      painter.save();
      painter.setClipRect(lane);
      QColor grid = palette.color(QPalette::Mid);
      grid.setAlpha(105);
      if (_laneMode == LaneMode::Velocity) {
            for (int guide : { 1, 32, 64, 96, 127 }) {
                  const int y = velocityToY(guide);
                  painter.setPen(QPen(grid, guide == 64 ? 1.0 : 0.7,
                                      guide == 64 ? Qt::DashLine : Qt::SolidLine));
                  painter.drawLine(lane.left(), y, lane.right(), y);
                  }
            const int baseline = velocityToY(1);
            const QVector<int> visible = _model->notesInRange(xToTick(lane.left()), xToTick(lane.right()), 0, 127);
            QVector<QRect> valueLabels;
            for (int index : visible) {
                  const KeyEditorModel::NoteBlock& block = _model->notes()[index];
                  const int x = velocityHandleX(block);
                  const int value = _velocityPreview.value(index, block.velocity);
                  const int y = velocityToY(value);
                  QColor color = noteColor(block.staffIdx, block.voice);
                  const bool selected = _model->eventSelected(index);
                  if (!selected)
                        color.setAlpha(165);
                  painter.setPen(QPen(color, selected ? 3.2 : 1.4));
                  painter.drawLine(x, baseline, x, y);
                  QColor selectedFill(
                        qRound(color.red() * 0.30 + 255 * 0.70),
                        qRound(color.green() * 0.30 + 255 * 0.70),
                        qRound(color.blue() * 0.30 + 255 * 0.70));
                  painter.setBrush(selected ? selectedFill : color);
                  if (selected) {
                        painter.setPen(QPen(color, 2.2));
                        painter.drawRect(QRectF(x - 5, y - 5, 10, 10));
                        const QString valueText = QString::number(value);
                        int labelX = x + 8;
                        if (labelX + 32 > lane.right())
                              labelX = x - 40;
                        const int labelY = qBound(lane.top() + 2, y - 9,
                                                  lane.bottom() - 19);
                        const QRect valueRect(labelX, labelY, 32, 18);
                        bool overlaps = false;
                        for (const QRect& existing : qAsConst(valueLabels)) {
                              if (existing.adjusted(-2, -1, 2, 1).intersects(valueRect)) {
                                    overlaps = true;
                                    break;
                                    }
                              }
                        if (!overlaps) {
                              valueLabels.append(valueRect);
                              painter.fillRect(valueRect, selectedFill);
                              painter.setPen(QColor(25, 25, 27));
                              painter.drawText(valueRect, Qt::AlignCenter, valueText);
                              }
                        }
                  else {
                        painter.setPen(color.darker(170));
                        painter.drawRect(QRectF(x - 3.5, y - 3.5, 7, 7));
                        }
                  }
            const QRect selectionBox = velocitySelectionRect();
            if (!selectionBox.isNull()) {
                  QColor boxColor = palette.color(QPalette::Text);
                  boxColor.setAlpha(185);
                  painter.fillRect(selectionBox.adjusted(1, 1, -1, -1),
                                   QColor(255, 255, 255, 32));
                  painter.setBrush(Qt::NoBrush);
                  painter.setPen(QPen(boxColor, 1.0, Qt::DashLine));
                  painter.drawRect(selectionBox);
                  const QPoint handles[] = {
                        selectionBox.topLeft(),
                        QPoint(selectionBox.center().x(), selectionBox.top()),
                        selectionBox.topRight()
                        };
                  painter.setBrush(QColor(232, 234, 238));
                  painter.setPen(QPen(QColor(75, 77, 82), 1.0));
                  for (const QPoint& handle : handles)
                        painter.drawRect(QRect(handle - QPoint(3, 3), QSize(7, 7)));
                  }
            if (_dragMode == DragMode::VelocityLine && _dragThresholdPassed) {
                  painter.setPen(QPen(palette.color(QPalette::Highlight), 2, Qt::DashLine));
                  painter.drawLine(_pressPos, _lastPos);
                  }
            }
      else if (_laneMode == LaneMode::Tempo) {
            const QRect graph = controllerContentRect().adjusted(0, 8, 0, -10);
            constexpr qreal minimumBpm = 20.0;
            constexpr qreal maximumBpm = 300.0;
            auto tempoY = [&graph](qreal bpm) {
                  const qreal ratio = (qBound(minimumBpm, bpm, maximumBpm) - minimumBpm)
                                    / (maximumBpm - minimumBpm);
                  return graph.bottom() - qRound(ratio * qMax(1, graph.height()));
                  };
            for (int bpm : { 60, 120, 180, 240, 300 }) {
                  const int y = tempoY(bpm);
                  painter.setPen(QPen(grid, bpm == 120 ? 1.0 : 0.7, Qt::DashLine));
                  painter.drawLine(lane.left(), y, lane.right(), y);
                  }
            if (_model->score() && _model->score()->tempomap()) {
                  TempoMap* tempos = _model->score()->tempomap();
                  QPainterPath path;
                  bool started = false;
                  for (int x = lane.left(); x <= lane.right(); x += 3) {
                        const qreal bpm = tempos->tempo(xToTick(x)) * 60.0;
                        const QPointF point(x, tempoY(bpm));
                        if (!started) {
                              path.moveTo(point);
                              started = true;
                              }
                        else
                              path.lineTo(point);
                        }
                  QColor tempoColor(229, 194, 59);
                  painter.setPen(QPen(tempoColor, 2.0));
                  painter.setBrush(Qt::NoBrush);
                  painter.drawPath(path);
                  const int firstTick = xToTick(lane.left());
                  const int lastTick = xToTick(lane.right());
                  for (auto it = tempos->lower_bound(firstTick); it != tempos->end() && it->first <= lastTick; ++it) {
                        if (!(it->second.type & TempoType::FIX))
                              continue;
                        const QPointF point(tickToX(it->first), tempoY(it->second.tempo * 60.0));
                        painter.setBrush(QColor(8, 8, 9));
                        painter.setPen(QPen(tempoColor, 2.0));
                        painter.drawEllipse(point, 4.5, 4.5);
                        painter.setPen(palette.color(QPalette::Text));
                        painter.drawText(QRectF(point.x() + 7, point.y() - 10, 54, 18),
                                         Qt::AlignLeft | Qt::AlignVCenter,
                                         tr("%1 BPM").arg(qRound(it->second.tempo * 60.0)));
                        }
                  }
            }
      else {
            const QList<Staff*>& staves = _model->visibleStaves();
            const int rowCount = qMax(1, staves.size());
            const QRect content = controllerContentRect().adjusted(0, 5, 0, -10);
            const int rowHeight = qMax(18, content.height() / rowCount);
            painter.setFont(QFont(painter.font().family(), 8));
            for (int row = 0; row < rowCount; ++row) {
                  const int y = content.top() + row * rowHeight;
                  painter.setPen(QPen(grid, 1));
                  painter.drawLine(content.left(), y, content.right(), y);
                  if (row < staves.size()) {
                        painter.setPen(staffColor(staves[row]->idx()));
                        painter.drawText(QRect(content.left() + 4, y + 2, 70, rowHeight - 4),
                                         Qt::AlignLeft | Qt::AlignTop,
                                         tr("Staff %1").arg(row + 1));
                        }
                  }
            for (const KeyEditorModel::PedalBlock& block : _model->pedals()) {
                  QRect rect = pedalRect(block);
                  if (!rect.intersects(lane))
                        continue;
                  QColor color = staffColor(block.staffIdx);
                  color.setAlpha(190);
                  painter.setBrush(color);
                  painter.setPen(QPen(block.spanner == _selectedPedal
                                      ? palette.color(QPalette::Highlight) : color.darker(175),
                                      block.spanner == _selectedPedal ? 2.2 : 1.2,
                                      block.letRing ? Qt::DashLine : Qt::SolidLine));
                  painter.drawRoundedRect(rect, 4, 4);
                  painter.setBrush(palette.color(QPalette::HighlightedText));
                  painter.drawEllipse(QPointF(rect.left(), rect.center().y()), 4, 4);
                  painter.drawEllipse(QPointF(rect.right(), rect.center().y()), 4, 4);
                  painter.setPen(Qt::white);
                  painter.drawText(rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                                   block.letRing ? tr("Let ring") : tr("Pedal"));
                  painter.drawText(rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignRight,
                                   tr("127 → 0"));
                  }
            if (_dragMode == DragMode::PedalCreate) {
                  KeyEditorModel::PedalBlock preview;
                  preview.startTick = _pedalPreviewStart;
                  preview.endTick = _pedalPreviewEnd;
                  preview.staffIdx = _editStaffIdx;
                  QRect rect = pedalRect(preview, false);
                  QColor color = staffColor(_editStaffIdx);
                  color.setAlpha(130);
                  painter.setBrush(color);
                  painter.setPen(QPen(palette.color(QPalette::Highlight), 2, Qt::DashLine));
                  painter.drawRoundedRect(rect, 4, 4);
                  }
            }
      painter.restore();
      }

void KeyEditorView::paintOverlays(QPainter& painter, const QRect& dirty)
      {
      Q_UNUSED(dirty);
      const QPalette palette = QApplication::palette();
      if (_dragMode == DragMode::Marquee) {
            painter.setPen(QPen(palette.color(QPalette::Highlight), 1.5));
            QColor fill = palette.color(QPalette::Highlight);
            fill.setAlpha(45);
            painter.setBrush(fill);
            painter.drawRect(_marquee.normalized());
            }
      for (int index = 1; index < 3; ++index) {
            if (_locatorTicks[index] < 0)
                  continue;
            const int x = tickToX(_locatorTicks[index]);
            painter.setPen(QPen(QColor(70, 145, 235, 190), 1.2));
            painter.drawLine(x, rulerHeight, x, viewport()->height());
            }
      if (_playbackTick >= 0) {
            const int x = tickToX(_playbackTick);
            painter.setPen(QPen(QColor(229, 67, 67), 1.7));
            painter.drawLine(x, 0, x, viewport()->height());
            QPolygon marker;
            marker << QPoint(x - 5, 0) << QPoint(x + 5, 0) << QPoint(x, 7);
            painter.setBrush(QColor(229, 67, 67));
            painter.drawPolygon(marker);
            }
      }

void KeyEditorView::paintEvent(QPaintEvent* event)
      {
      QPainter painter(viewport());
      painter.setRenderHint(QPainter::Antialiasing, true);
      const QRect dirty = event->rect();
      paintBackground(painter, dirty);
      paintGrid(painter, dirty);
      paintRuler(painter, dirty);
      paintKeyboard(painter, dirty);
      paintNotes(painter, dirty);
      paintControllerLane(painter, dirty);
      paintOverlays(painter, dirty);
      }

void KeyEditorView::resizeEvent(QResizeEvent* event)
      {
      QAbstractScrollArea::resizeEvent(event);
      setControllerHeight(_controllerHeight);
      updateScrollBars();
      updateLaneControlGeometry();
      }

void KeyEditorView::scrollContentsBy(int dx, int dy)
      {
      Q_UNUSED(dx);
      Q_UNUSED(dy);
      // Fixed headers and child controls share this viewport with scrolling
      // content. Bit-blitting the viewport copies those fixed layers too and
      // produces stale text, playhead trails, and distorted combo boxes.
      viewport()->update();
      updateLaneControlGeometry();
      }

QRect KeyEditorView::previewRegion() const
      {
      QRect result;
      bool initialized = false;
      for (const KeyEditorModel::NoteEdit& edit : _previewEdits) {
            QRect rect = editRect(edit).toAlignedRect().adjusted(-5, -5, 5, 5);
            result = initialized ? result.united(rect) : rect;
            initialized = true;
            }
      if (_dragMode == DragMode::DrawNote && _dragThresholdPassed) {
            QRect rect = editRect(_drawPreview).toAlignedRect().adjusted(-5, -5, 5, 5);
            result = initialized ? result.united(rect) : rect;
            initialized = true;
            }
      if (_dragMode == DragMode::Marquee) {
            result = initialized ? result.united(_marquee.normalized().adjusted(-2, -2, 2, 2))
                                 : _marquee.normalized().adjusted(-2, -2, 2, 2);
            }
      return result;
      }

void KeyEditorView::updateDragRegion(const QRect& oldRegion)
      {
      QRect dirty = previewRegion();
      if (!oldRegion.isNull())
            dirty = dirty.isNull() ? oldRegion : dirty.united(oldRegion);
      if (!dirty.isNull())
            viewport()->update(dirty.intersected(viewport()->rect()));
      }

void KeyEditorView::stopAudition()
      {
      if (_auditionPitch < 0)
            return;
      emit pitchReleased(_auditionPitch);
      _auditionPitch = -1;
      _auditionStaffIdx = -1;
      }

void KeyEditorView::cancelActiveGesture()
      {
      stopAudition();
      _autoScrollTimer->stop();
      _dragMode = DragMode::None;
      _dragThresholdPassed = false;
      _duplicateDrag = false;
      _noteDragAnchor = nullptr;
      _lastDragPreviewPitch = -1;
      _originalEdits.clear();
      _previewEdits.clear();
      _previewBlockIndexes.clear();
      _velocityOriginal.clear();
      _velocityPreview.clear();
      _velocityAnchor = -1;
      _velocityTransformRect = QRect();
      _marquee = QRect();
      setEditTool(_editTool);
      viewport()->update();
      }

void KeyEditorView::auditionPitchBriefly(int pitch, int staffIdx)
      {
      stopAudition();
      _auditionPitch = qBound(0, pitch, 127);
      _auditionStaffIdx = staffIdx;
      const int releasePitch = _auditionPitch;
      emit pitchPressed(_auditionPitch, _auditionStaffIdx);
      QTimer::singleShot(180, this, [this, releasePitch]() {
            if (_auditionPitch == releasePitch)
                  stopAudition();
            });
      }

bool KeyEditorView::shouldAutoScrollForDrag(const QPoint& point) const
      {
      if (_dragMode == DragMode::None || _dragMode == DragMode::Pan
          || _dragMode == DragMode::ResizeLane
          || _dragMode == DragMode::VelocityBoxUniform
          || _dragMode == DragMode::VelocityBoxLeft
          || _dragMode == DragMode::VelocityBoxRight)
            return false;
      if (!_dragThresholdPassed
          && QLineF(_pressPos, point).length() < QApplication::startDragDistance())
            return false;

      const bool laneGesture = _dragMode == DragMode::VelocityHandle
            || _dragMode == DragMode::VelocityFreehand
            || _dragMode == DragMode::VelocityLine
            || _dragMode == DragMode::VelocityBoxUniform
            || _dragMode == DragMode::VelocityBoxLeft
            || _dragMode == DragMode::VelocityBoxRight
            || _dragMode == DragMode::PedalCreate
            || _dragMode == DragMode::PedalMove
            || _dragMode == DragMode::PedalStart
            || _dragMode == DragMode::PedalEnd;
      const QRect area = laneGesture ? controllerContentRect() : noteAreaRect();
      constexpr int margin = 24;
      const bool horizontalEdge = point.x() < area.left() + margin
                               || point.x() > area.right() - margin;
      const bool verticalEdge = !laneGesture
                             && (point.y() < area.top() + margin
                                 || point.y() > area.bottom() - margin);
      return horizontalEdge || verticalEdge;
      }

bool KeyEditorView::autoScrollForDrag(const QPoint& point)
      {
      if (!shouldAutoScrollForDrag(point))
            return false;

      const bool laneGesture = _dragMode == DragMode::VelocityHandle
            || _dragMode == DragMode::VelocityFreehand
            || _dragMode == DragMode::VelocityLine
            || _dragMode == DragMode::VelocityBoxUniform
            || _dragMode == DragMode::VelocityBoxLeft
            || _dragMode == DragMode::VelocityBoxRight
            || _dragMode == DragMode::PedalCreate
            || _dragMode == DragMode::PedalMove
            || _dragMode == DragMode::PedalStart
            || _dragMode == DragMode::PedalEnd;
      const QRect area = laneGesture ? controllerContentRect() : noteAreaRect();
      constexpr int margin = 24;
      const int oldHorizontal = horizontalScrollBar()->value();
      if (point.x() < area.left() + margin) {
            const int step = qBound(8, area.left() + margin - point.x(), 32);
            horizontalScrollBar()->setValue(oldHorizontal - step);
            }
      else if (point.x() > area.right() - margin) {
            const int step = qBound(8, point.x() - (area.right() - margin), 32);
            horizontalScrollBar()->setValue(oldHorizontal + step);
            }

      const int oldVertical = verticalScrollBar()->value();
      if (!laneGesture) {
            if (point.y() < area.top() + margin) {
                  const int step = qBound(8, area.top() + margin - point.y(), 32);
                  verticalScrollBar()->setValue(oldVertical - step);
                  }
            else if (point.y() > area.bottom() - margin) {
                  const int step = qBound(8, point.y() - (area.bottom() - margin), 32);
                  verticalScrollBar()->setValue(oldVertical + step);
                  }
            }
      return oldHorizontal != horizontalScrollBar()->value()
             || oldVertical != verticalScrollBar()->value();
      }

void KeyEditorView::refreshActiveDragAfterScroll()
      {
      switch (_dragMode) {
            case DragMode::Marquee: {
                  const QRect oldRegion = previewRegion();
                  const QPoint anchor(_marqueeContentAnchor.x() - horizontalScrollBar()->value(),
                                      _marqueeContentAnchor.y() - verticalScrollBar()->value());
                  _marqueeEndTick = xToTick(_lastPos.x());
                  _marqueeEndPitch = yToPitch(_lastPos.y());
                  _marquee = QRect(anchor, _lastPos).normalized().intersected(noteAreaRect());
                  updateDragRegion(oldRegion);
                  break;
                  }
            case DragMode::MoveNotes:
            case DragMode::ResizeLeft:
            case DragMode::ResizeRight:
                  updateNoteDrag(_lastPos, _pressModifiers);
                  break;
            case DragMode::DrawNote:
                  drawNoteAt(_lastPos, false);
                  break;
            case DragMode::VelocityHandle:
            case DragMode::VelocityFreehand:
            case DragMode::VelocityLine:
            case DragMode::VelocityBoxUniform:
            case DragMode::VelocityBoxLeft:
            case DragMode::VelocityBoxRight:
                  updateVelocityGesture(_lastPos);
                  break;
            case DragMode::PedalCreate:
            case DragMode::PedalMove:
            case DragMode::PedalStart:
            case DragMode::PedalEnd:
                  updatePedalGesture(_lastPos);
                  break;
            default:
                  break;
            }
      }

void KeyEditorView::updateCursorForPosition(const QPoint& point)
      {
      if (laneSeparatorRect().contains(point)) {
            viewport()->setCursor(Qt::SplitVCursor);
            return;
            }
      if (controllerContentRect().contains(point)) {
            if (_laneMode == LaneMode::Velocity) {
                  const DragMode boxHandle = _laneTool == LaneTool::Pointer
                                           ? velocitySelectionHandleAt(point)
                                           : DragMode::None;
                  if (boxHandle == DragMode::VelocityBoxLeft
                      || boxHandle == DragMode::VelocityBoxRight)
                        viewport()->setCursor(Qt::SizeBDiagCursor);
                  else if (boxHandle == DragMode::VelocityBoxUniform
                           || (_laneTool == LaneTool::Pointer && velocityHandleAt(point) >= 0))
                        viewport()->setCursor(Qt::SizeVerCursor);
                  else
                        viewport()->setCursor(Qt::CrossCursor);
                  }
            else if (_laneMode == LaneMode::Sustain) {
                  DragMode part = DragMode::None;
                  if (pedalAt(point, &part))
                        viewport()->setCursor(part == DragMode::PedalMove
                                              ? Qt::SizeAllCursor : Qt::SizeHorCursor);
                  else
                        viewport()->setCursor(_laneTool == LaneTool::Freehand
                                              ? Qt::CrossCursor : Qt::ArrowCursor);
                  }
            else
                  viewport()->setCursor(Qt::ArrowCursor);
            return;
            }
      if (noteAreaRect().contains(point)) {
            if (_editTool == EditTool::Draw) {
                  viewport()->setCursor(Qt::CrossCursor);
                  return;
                  }
            if (_editTool == EditTool::Erase) {
                  viewport()->setCursor(Qt::ForbiddenCursor);
                  return;
                  }
            const int index = noteAtPoint(point);
            if (index >= 0) {
                  const QRectF rect = noteRect(_model->notes()[index]);
                  const qreal edge = qMin<qreal>(7.0, qMax<qreal>(3.0, rect.width() / 4.0));
                  viewport()->setCursor(qAbs(point.x() - rect.left()) <= edge
                                        || qAbs(point.x() - rect.right()) <= edge
                                        ? Qt::SizeHorCursor : Qt::SizeAllCursor);
                  }
            else
                  viewport()->setCursor(Qt::ArrowCursor);
            return;
            }
      viewport()->setCursor(Qt::ArrowCursor);
      }

void KeyEditorView::selectNoteForClick(int noteIndex, Qt::KeyboardModifiers modifiers)
      {
      if (!_model || noteIndex < 0 || noteIndex >= _model->notes().size())
            return;
      Note* note = _model->notes()[noteIndex].note;
      const bool wasSelected = _model->eventSelected(noteIndex);
      const bool willDeselect = wasSelected && (modifiers & Qt::ControlModifier);
      if (!wasSelected && _model->score())
            _model->score()->setPlayNote(true);
      if (!(modifiers & Qt::ControlModifier) || !wasSelected)
            emit noteFocusRequested(note);
      if (modifiers & Qt::ControlModifier)
            _model->selectEvents({ noteIndex }, KeyEditorModel::SelectionOperation::Toggle);
      else if (modifiers & Qt::ShiftModifier)
            _model->selectEvents({ noteIndex }, KeyEditorModel::SelectionOperation::Add);
      else if (!wasSelected)
            _model->selectEvents({ noteIndex }, KeyEditorModel::SelectionOperation::Replace);
      // A newly selected note is previewed by MuseScoreCore::endCmd(), exactly
      // like a score click. An already-selected note has no selection command,
      // so invoke the same MuseScore preview function directly.
      if (wasSelected && !willDeselect && mscore)
            mscore->play(note);
      }

bool KeyEditorView::commitNoteEdits(const QVector<KeyEditorModel::NoteEdit>& edits, bool duplicate)
      {
      if (!_model)
            return false;
      return _model->applyPlaybackTimingEdits(edits, duplicate);
      }

bool KeyEditorView::nudgeSelection(int tickDelta, int pitchDelta, bool duplicate)
      {
      if (!_model)
            return false;
      if (duplicate)
            return _model->nudgeSelection(tickDelta, pitchDelta, duplicate);

      QVector<KeyEditorModel::NoteEdit> edits;
      int minimumStart = INT_MAX;
      for (int index : _model->selectedEventIndexes()) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            minimumStart = qMin(minimumStart, block.startTick);
            edits.append({ block.note, block.startTick + tickDelta, block.endTick + tickDelta,
                           block.pitch + pitchDelta, block.staffIdx, block.voice,
                           block.eventIndex });
            }
      if (minimumStart != INT_MAX && minimumStart + tickDelta < 0) {
            const int correction = -(minimumStart + tickDelta);
            for (KeyEditorModel::NoteEdit& edit : edits) {
                  edit.startTick += correction;
                  edit.endTick += correction;
                  }
            }
      return commitNoteEdits(edits);
      }

bool KeyEditorView::quantizeSelection()
      {
      if (!_model || _gridTicks <= 0)
            return false;
      QVector<KeyEditorModel::NoteEdit> edits;
      for (int index : _model->selectedEventIndexes()) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            const int start = qMax(0, int(std::floor((block.startTick + _gridTicks / 2.0)
                                                     / _gridTicks)) * _gridTicks);
            edits.append({ block.note, start, start + block.endTick - block.startTick,
                           block.pitch, block.staffIdx, block.voice, block.eventIndex });
            }
      return commitNoteEdits(edits);
      }

void KeyEditorView::beginNoteDrag(int noteIndex, const QPoint& point)
      {
      if (!_model || noteIndex < 0 || noteIndex >= _model->notes().size())
            return;
      const KeyEditorModel::NoteBlock& hit = _model->notes()[noteIndex];
      const QRectF hitRect = noteRect(hit);
      if (!_model->eventSelected(noteIndex))
            return;

      const qreal edgeWidth = qMin<qreal>(7.0, qMax<qreal>(3.0, hitRect.width() / 4.0));
      if (qAbs(point.x() - hitRect.left()) <= edgeWidth)
            _dragMode = DragMode::ResizeLeft;
      else if (qAbs(point.x() - hitRect.right()) <= edgeWidth)
            _dragMode = DragMode::ResizeRight;
      else
            _dragMode = DragMode::MoveNotes;

      _originalEdits.clear();
      _previewEdits.clear();
      _previewBlockIndexes.clear();
      for (int index : _model->selectedEventIndexes()) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            KeyEditorModel::NoteEdit edit;
            edit.source = block.note;
            edit.eventIndex = block.eventIndex;
            edit.startTick = block.startTick;
            edit.endTick = block.endTick;
            edit.pitch = block.pitch;
            edit.staffIdx = block.staffIdx;
            edit.voice = block.voice;
            _originalEdits.append(edit);
            _previewBlockIndexes.insert(index);
            }
      _previewEdits = _originalEdits;
      _noteDragAnchor = hit.note;
      _duplicateDrag = _pressModifiers & Qt::AltModifier;
      stopAudition();
      _lastDragPreviewPitch = hit.pitch;
      }

void KeyEditorView::updateNoteDrag(const QPoint& point, Qt::KeyboardModifiers modifiers)
      {
      if (_originalEdits.isEmpty())
            return;
      if (!_dragThresholdPassed
          && QLineF(_pressPos, point).length() < QApplication::startDragDistance())
            return;
      _dragThresholdPassed = true;
      const QRect oldRegion = previewRegion();
      _pressModifiers = modifiers;
      const int rawTickDelta = xToTick(point.x()) - _pressTick;
      int tickDelta = rawTickDelta;
      int pitchDelta = yToPitch(point.y()) - _pressPitch;
      const int horizontalPixels = qAbs(point.x() - _pressPos.x());
      const int verticalPixels = qAbs(point.y() - _pressPos.y());
      const KeyEditorModel::NoteEdit* anchor = &_originalEdits.front();
      for (const KeyEditorModel::NoteEdit& edit : qAsConst(_originalEdits)) {
            if (edit.source == _noteDragAnchor) {
                  anchor = &edit;
                  break;
                  }
            }

      // A vertical pitch drag must not also snap an off-grid note to a new time
      // merely because the mouse moved by a pixel or two horizontally.  That
      // accidental structural edit could be rejected while the pitch-only edit
      // itself was perfectly valid.
      const bool pitchOnlyGesture = _dragMode == DragMode::MoveNotes
                                 && (horizontalPixels < QApplication::startDragDistance()
                                     || verticalPixels > horizontalPixels * 2);
      if (pitchOnlyGesture)
            tickDelta = 0;
      else if (_snapEnabled && !(modifiers & Qt::ShiftModifier)) {
            const int target = _dragMode == DragMode::ResizeRight
                             ? anchor->endTick + rawTickDelta : anchor->startTick + rawTickDelta;
            const int snapped = snapTick(target);
            tickDelta = snapped - (_dragMode == DragMode::ResizeRight
                                   ? anchor->endTick : anchor->startTick);
            }
      if (_dragMode == DragMode::MoveNotes && (modifiers & Qt::ControlModifier)) {
            const int dx = qAbs(point.x() - _pressPos.x());
            const int dy = qAbs(point.y() - _pressPos.y());
            if (dx >= dy)
                  pitchDelta = 0;
            else
                  tickDelta = 0;
            }

      int minimumStart = INT_MAX;
      for (const KeyEditorModel::NoteEdit& edit : _originalEdits)
            minimumStart = qMin(minimumStart, edit.startTick);
      if (_dragMode == DragMode::MoveNotes && minimumStart + tickDelta < 0)
            tickDelta = -minimumStart;

      _previewEdits = _originalEdits;
      for (KeyEditorModel::NoteEdit& edit : _previewEdits) {
            switch (_dragMode) {
                  case DragMode::MoveNotes:
                        edit.startTick += tickDelta;
                        edit.endTick += tickDelta;
                        edit.pitch = qBound(0, edit.pitch + pitchDelta, 127);
                        break;
                  case DragMode::ResizeLeft:
                        edit.startTick = qBound(0, edit.startTick + tickDelta, edit.endTick - 1);
                        break;
                  case DragMode::ResizeRight:
                        edit.endTick = qMax(edit.startTick + 1, edit.endTick + tickDelta);
                        break;
                  default:
                        break;
                  }
            }
      if (_dragMode == DragMode::MoveNotes && !_previewEdits.isEmpty()) {
            const int pitch = _previewEdits.front().pitch;
            if (pitchDelta != 0 && pitch != _lastDragPreviewPitch
                && mscore && anchor->source) {
                  mscore->play(anchor->source, pitch);
                  _lastDragPreviewPitch = pitch;
                  }
            }
      _lastPos = point;
      updateDragRegion(oldRegion);
      }

void KeyEditorView::finishNoteDrag()
      {
      stopAudition();
      QRegion damage;
      for (const KeyEditorModel::NoteEdit& edit : qAsConst(_originalEdits))
            damage += editRect(edit).toAlignedRect().adjusted(-5, -5, 5, 5);
      for (const KeyEditorModel::NoteEdit& edit : qAsConst(_previewEdits))
            damage += editRect(edit).toAlignedRect().adjusted(-5, -5, 5, 5);
      if (_dragThresholdPassed && _model && !_previewEdits.isEmpty())
            commitNoteEdits(_previewEdits, _duplicateDrag);
      _originalEdits.clear();
      _previewEdits.clear();
      _previewBlockIndexes.clear();
      _noteDragAnchor = nullptr;
      _lastDragPreviewPitch = -1;
      _duplicateDrag = false;
      if (!damage.isEmpty())
            viewport()->update(damage.intersected(noteAreaRect()));
      }

void KeyEditorView::beginMarquee(const QPoint& point)
      {
      _dragMode = DragMode::Marquee;
      _marquee = QRect(point, point);
      _marqueeContentAnchor = QPoint(point.x() + horizontalScrollBar()->value(),
                                     point.y() + verticalScrollBar()->value());
      _marqueeEndTick = _pressTick;
      _marqueeEndPitch = _pressPitch;
      }

void KeyEditorView::finishMarquee()
      {
      if (!_model)
            return;
      emit pianoRollSelectionStarted();
      const QRect visibleRect = _marquee.normalized().intersected(noteAreaRect());
      const QRect damage = visibleRect.adjusted(-3, -3, 3, 3);
      if (!_dragThresholdPassed) {
            if (!(_pressModifiers & (Qt::ControlModifier | Qt::ShiftModifier)))
                  _model->clearSelection();
            }
      else {
            KeyEditorModel::SelectionOperation operation = KeyEditorModel::SelectionOperation::Replace;
            if (_pressModifiers & Qt::ControlModifier)
                  operation = KeyEditorModel::SelectionOperation::Toggle;
            else if (_pressModifiers & Qt::ShiftModifier)
                  operation = KeyEditorModel::SelectionOperation::Add;
            _model->selectRange(qMin(_pressTick, _marqueeEndTick),
                                qMax(_pressTick, _marqueeEndTick),
                                qMin(_pressPitch, _marqueeEndPitch),
                                qMax(_pressPitch, _marqueeEndPitch), operation);
            }
      _marquee = QRect();
      if (!damage.isNull())
            viewport()->update(damage.intersected(noteAreaRect()));
      }

void KeyEditorView::drawNoteAt(const QPoint& point, bool commit)
      {
      if (!_model || !_model->editStaff())
            return;
      if (_dragMode != DragMode::DrawNote) {
            _dragMode = DragMode::DrawNote;
            const int tick = (_pressModifiers & Qt::ShiftModifier) ? xToTick(point.x())
                                                                    : snapTick(xToTick(point.x()));
            _drawAnchorTick = tick;
            _drawAnchorPitch = yToPitch(point.y());
            _drawPreview.source = nullptr;
            _drawPreview.startTick = tick;
            _drawPreview.endTick = tick;
            _drawPreview.pitch = _drawAnchorPitch;
            _drawPreview.staffIdx = _editStaffIdx;
            _drawPreview.voice = _editVoice;
            }
      else if (!commit) {
            if (!_dragThresholdPassed
                && QLineF(_pressPos, point).length() < QApplication::startDragDistance())
                  return;
            _dragThresholdPassed = true;
            const QRect oldRegion = previewRegion();
            int tick = (_pressModifiers & Qt::ShiftModifier) ? xToTick(point.x())
                                                               : snapTick(xToTick(point.x()));
            if (tick >= _drawAnchorTick) {
                  _drawPreview.startTick = _drawAnchorTick;
                  _drawPreview.endTick = tick;
                  }
            else {
                  _drawPreview.startTick = tick;
                  _drawPreview.endTick = _drawAnchorTick;
                  }
            _drawPreview.pitch = _drawAnchorPitch;
            updateDragRegion(oldRegion);
            }
      if (commit) {
            if (_dragThresholdPassed && _drawPreview.endTick > _drawPreview.startTick
                && _model->createNote(_drawPreview.startTick,
                                   _drawPreview.endTick - _drawPreview.startTick,
                                   _drawPreview.pitch, _drawPreview.staffIdx, _drawPreview.voice)) {
                  auditionPitchBriefly(_drawPreview.pitch, _drawPreview.staffIdx);
                  }
            }
      }

int KeyEditorView::velocityHandleAt(const QPoint& point) const
      {
      if (!_model)
            return -1;
      const int tickRadius = qMax(1, qRound(8.0 / _pixelsPerTick));
      int best = -1;
      int bestDistance = 1000;
      for (int index : _model->notesInRange(xToTick(point.x()) - tickRadius,
                                            xToTick(point.x()) + tickRadius, 0, 127)) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            const int dx = velocityHandleX(block) - point.x();
            const int value = _velocityPreview.value(index, block.velocity);
            const int dy = velocityToY(value) - point.y();
            const int distance = dx * dx + dy * dy;
            if (distance <= 81 && distance < bestDistance) {
                  best = index;
                  bestDistance = distance;
                  }
            }
      return best;
      }

void KeyEditorView::beginVelocityGesture(const QPoint& point)
      {
      if (!_model)
            return;
      _velocityOriginal.clear();
      _velocityPreview.clear();
      const DragMode selectionHandle = _laneTool == LaneTool::Pointer
                                     ? velocitySelectionHandleAt(point)
                                     : DragMode::None;
      if (selectionHandle != DragMode::None) {
            _dragMode = selectionHandle;
            _velocityTransformRect = velocitySelectionRect();
            for (int index : _model->selectedEventIndexes())
                  _velocityOriginal.insert(index, _model->notes()[index].velocity);
            _velocityPreview = _velocityOriginal;
            return;
            }
      _velocityAnchor = velocityHandleAt(point);
      if (_laneTool == LaneTool::Pointer && _velocityAnchor >= 0) {
            selectNoteForClick(_velocityAnchor, _pressModifiers);
            if (!_model->eventSelected(_velocityAnchor))
                  return;
            const QVector<int> selected = _model->selectedEventIndexes();
            for (int index : selected)
                  _velocityOriginal.insert(index, _model->notes()[index].velocity);
            _velocityPreview = _velocityOriginal;
            _pressVelocity = _model->notes()[_velocityAnchor].velocity;
            _dragMode = DragMode::VelocityHandle;
            }
      else if (_laneTool == LaneTool::Freehand) {
            _dragMode = DragMode::VelocityFreehand;
            updateVelocityFreehand(point, point);
            }
      else if (_laneTool == LaneTool::Line) {
            _dragMode = DragMode::VelocityLine;
            }
      }

void KeyEditorView::updateVelocityFreehand(const QPoint& from, const QPoint& to)
      {
      if (!_model)
            return;
      // Use a screen-space brush instead of requiring an exact onset tick.
      // This makes clicks and mostly vertical strokes edit nearby handles too,
      // and intentionally applies to selected and unselected notes alike.
      constexpr int brushRadius = 6;
      const int left = qMin(from.x(), to.x()) - brushRadius;
      const int right = qMax(from.x(), to.x()) + brushRadius;
      const int start = xToTick(left);
      const int end = xToTick(right);
      for (int index : _model->notesInRange(start, end, 0, 127)) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            if (!block.note)
                  continue;
            const int x = velocityHandleX(block);
            if (x < left || x > right)
                  continue;
            const qreal ratio = to.x() == from.x() ? 1.0
                              : qBound<qreal>(0.0,
                                    (x - from.x()) / qreal(to.x() - from.x()), 1.0);
            const int y = qRound(from.y() + ratio * (to.y() - from.y()));
            _velocityPreview.insert(index, yToVelocity(y));
            }
      }

void KeyEditorView::updateVelocityLine(const QPoint& from, const QPoint& to)
      {
      if (!_model)
            return;
      _velocityPreview.clear();
      const int tick0 = xToTick(from.x());
      const int tick1 = xToTick(to.x());
      const int start = qMin(tick0, tick1);
      const int end = qMax(tick0, tick1);
      for (int index : _model->notesInRange(start, end, 0, 127)) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            if (block.startTick < start || block.startTick > end)
                  continue;
            const qreal ratio = tick1 == tick0 ? 1.0 : (block.startTick - tick0) / qreal(tick1 - tick0);
            const int y = qRound(from.y() + ratio * (to.y() - from.y()));
            _velocityPreview.insert(index, yToVelocity(y));
            }
      }

void KeyEditorView::updateVelocityGesture(const QPoint& point)
      {
      if (!_model)
            return;
      const QHash<int, int> oldPreview = _velocityPreview;
      const QPoint oldPoint = _lastPos;
      _dragThresholdPassed = _dragThresholdPassed
                          || QLineF(_pressPos, point).length() >= QApplication::startDragDistance();
      switch (_dragMode) {
            case DragMode::VelocityHandle: {
                  const int target = yToVelocity(point.y());
                  const int delta = target - _pressVelocity;
                  _velocityPreview.clear();
                  for (auto it = _velocityOriginal.constBegin(); it != _velocityOriginal.constEnd(); ++it)
                        _velocityPreview.insert(it.key(), qBound(1, it.value() + delta, 127));
                  break;
                  }
            case DragMode::VelocityFreehand:
                  updateVelocityFreehand(_lastPos, point);
                  break;
            case DragMode::VelocityLine:
                  updateVelocityLine(_pressPos, point);
                  break;
            case DragMode::VelocityBoxUniform:
            case DragMode::VelocityBoxLeft:
            case DragMode::VelocityBoxRight: {
                  const int baseline = velocityToY(1);
                  const int originalHeight = qMax(1, baseline - _velocityTransformRect.top());
                  const int targetHeight = qMax(1, originalHeight + _pressPos.y() - point.y());
                  const qreal edgeFactor = targetHeight / qreal(originalHeight);
                  const qreal width = qMax(1, _velocityTransformRect.width());
                  const QRect graph = controllerContentRect().adjusted(0, 0, 0, -10);
                  const int uniformDelta = qRound((_pressPos.y() - point.y()) * 126.0
                                                  / qMax(1, graph.height() - 1));
                  _velocityPreview.clear();
                  for (auto it = _velocityOriginal.constBegin(); it != _velocityOriginal.constEnd(); ++it) {
                        if (_dragMode == DragMode::VelocityBoxUniform) {
                              _velocityPreview.insert(it.key(),
                                    qBound(1, it.value() + uniformDelta, 127));
                              continue;
                              }
                        qreal weight = 1.0;
                        const int index = it.key();
                        if (index >= 0) {
                              const int x = velocityHandleX(_model->notes()[index]);
                              const qreal position = qBound<qreal>(0.0,
                                    (x - _velocityTransformRect.left()) / width, 1.0);
                              weight = _dragMode == DragMode::VelocityBoxLeft
                                     ? 1.0 - position : position;
                              }
                        const qreal factor = 1.0 + (edgeFactor - 1.0) * weight;
                        _velocityPreview.insert(it.key(),
                              qBound(1, qRound(it.value() * factor), 127));
                        }
                  break;
                  }
            default:
                  break;
            }
      _lastPos = point;
      QRegion dirty = velocityGestureRegion(oldPreview);
      dirty += velocityGestureRegion(_velocityPreview);
      if (_dragMode == DragMode::VelocityLine) {
            dirty += QRect(_pressPos, oldPoint).normalized().adjusted(-4, -4, 4, 4);
            dirty += QRect(_pressPos, point).normalized().adjusted(-4, -4, 4, 4);
            }
      else if (_dragMode == DragMode::VelocityBoxUniform
               || _dragMode == DragMode::VelocityBoxLeft
               || _dragMode == DragMode::VelocityBoxRight) {
            dirty += controllerContentRect();
            }
      if (_model->selectedEventIndexes().size() >= 2)
            dirty += controllerContentRect();
      if (!dirty.isEmpty())
            viewport()->update(dirty);
      }

void KeyEditorView::finishVelocityGesture()
      {
      const QRegion oldPreview = velocityGestureRegion(_velocityPreview);
      const bool clickDraw = _dragMode == DragMode::VelocityFreehand;
      if (_model && !_velocityPreview.isEmpty() && (_dragThresholdPassed || clickDraw))
            _model->setEventVelocities(_velocityPreview);
      _velocityOriginal.clear();
      _velocityPreview.clear();
      _velocityAnchor = -1;
      _velocityTransformRect = QRect();
      if (!oldPreview.isEmpty())
            viewport()->update(oldPreview);
      }

Spanner* KeyEditorView::pedalAt(const QPoint& point, DragMode* part) const
      {
      if (!_model)
            return nullptr;
      for (auto it = _model->pedals().crbegin(); it != _model->pedals().crend(); ++it) {
            QRect rect = pedalRect(*it);
            if (!rect.adjusted(-5, -4, 5, 4).contains(point))
                  continue;
            if (part) {
                  if (qAbs(point.x() - rect.left()) <= 7)
                        *part = DragMode::PedalStart;
                  else if (qAbs(point.x() - rect.right()) <= 7)
                        *part = DragMode::PedalEnd;
                  else
                        *part = DragMode::PedalMove;
                  }
            return it->spanner;
            }
      return nullptr;
      }

void KeyEditorView::beginPedalGesture(const QPoint& point)
      {
      if (!_model)
            return;
      DragMode part = DragMode::None;
      Spanner* hit = pedalAt(point, &part);
      if (hit) {
            _selectedPedal = hit;
            _dragMode = part;
            for (const KeyEditorModel::PedalBlock& block : _model->pedals()) {
                  if (block.spanner == hit) {
                        _pedalOriginalStart = block.startTick;
                        _pedalOriginalEnd = block.endTick;
                        _pedalPreviewStart = block.startTick;
                        _pedalPreviewEnd = block.endTick;
                        break;
                        }
                  }
            }
      else if (_laneTool == LaneTool::Freehand) {
            _selectedPedal = nullptr;
            _dragMode = DragMode::PedalCreate;
            const int tick = (_pressModifiers & Qt::ShiftModifier) ? xToTick(point.x())
                                                                    : snapTick(xToTick(point.x()));
            _pedalCreateAnchor = tick;
            _pedalPreviewStart = tick;
            _pedalPreviewEnd = tick + qMax(1, _gridTicks);
            }
      else {
            _selectedPedal = nullptr;
            _dragMode = DragMode::None;
            }
      viewport()->update(laneRect());
      }

void KeyEditorView::updatePedalGesture(const QPoint& point)
      {
      if (_dragMode == DragMode::PedalCreate && !_dragThresholdPassed) {
            if (QLineF(_pressPos, point).length() < QApplication::startDragDistance())
                  return;
            _dragThresholdPassed = true;
            }
      const int rawDelta = xToTick(point.x()) - _pressTick;
      int delta = rawDelta;
      if (_snapEnabled && !(_pressModifiers & Qt::ShiftModifier)) {
            int anchor = _pedalOriginalStart;
            if (_dragMode == DragMode::PedalEnd)
                  anchor = _pedalOriginalEnd;
            delta = snapTick(anchor + rawDelta) - anchor;
            }
      switch (_dragMode) {
            case DragMode::PedalCreate: {
                  const int tick = (_pressModifiers & Qt::ShiftModifier) ? xToTick(point.x())
                                                                          : snapTick(xToTick(point.x()));
                  if (tick >= _pedalCreateAnchor) {
                        _pedalPreviewStart = _pedalCreateAnchor;
                        _pedalPreviewEnd = qMax(_pedalCreateAnchor + qMax(1, _gridTicks), tick);
                        }
                  else {
                        _pedalPreviewStart = tick;
                        _pedalPreviewEnd = _pedalCreateAnchor;
                        }
                  break;
                  }
            case DragMode::PedalMove: {
                  const int length = _pedalOriginalEnd - _pedalOriginalStart;
                  _pedalPreviewStart = qMax(0, _pedalOriginalStart + delta);
                  _pedalPreviewEnd = _pedalPreviewStart + length;
                  break;
                  }
            case DragMode::PedalStart:
                  _pedalPreviewStart = qBound(0, _pedalOriginalStart + delta, _pedalOriginalEnd - 1);
                  break;
            case DragMode::PedalEnd:
                  _pedalPreviewEnd = qMax(_pedalOriginalStart + 1, _pedalOriginalEnd + delta);
                  break;
            default:
                  break;
            }
      viewport()->update(laneRect());
      }

void KeyEditorView::finishPedalGesture()
      {
      if (!_model)
            return;
      if (_dragMode == DragMode::PedalCreate) {
            if (_pedalPreviewEnd <= _pedalPreviewStart)
                  _pedalPreviewEnd = _pedalPreviewStart + 1;
            _model->createPedal(_pedalPreviewStart, _pedalPreviewEnd, _editStaffIdx);
            }
      else if (_selectedPedal)
            _model->editPedal(_selectedPedal, _pedalPreviewStart, _pedalPreviewEnd);
      viewport()->update(laneRect());
      }

void KeyEditorView::mousePressEvent(QMouseEvent* event)
      {
      setFocus(Qt::MouseFocusReason);
      _pressPos = _lastPos = event->pos();
      _pressTick = xToTick(event->pos().x());
      _pressPitch = yToPitch(event->pos().y());
      _pressModifiers = event->modifiers();
      _dragThresholdPassed = false;

      if (event->button() == Qt::MiddleButton) {
            _dragMode = DragMode::Pan;
            _panStart = event->pos();
            _panStartH = horizontalScrollBar()->value();
            _panStartV = verticalScrollBar()->value();
            viewport()->setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
            }
      if (event->button() != Qt::LeftButton) {
            QAbstractScrollArea::mousePressEvent(event);
            return;
            }
      if (rulerRect().contains(event->pos())) {
            _focusDomain = FocusDomain::Notes;
            _cursorTick = xToTick(event->pos().x());
            emit seekRequested(_cursorTick);
            event->accept();
            return;
            }
      if (keyboardRect().contains(event->pos())) {
            _focusDomain = FocusDomain::Notes;
            stopAudition();
            _auditionPitch = yToPitch(event->pos().y());
            _auditionStaffIdx = _editStaffIdx;
            emit pitchPressed(_auditionPitch, _auditionStaffIdx);
            event->accept();
            return;
            }
      if (laneSeparatorRect().contains(event->pos())) {
            _dragMode = DragMode::ResizeLane;
            viewport()->setCursor(Qt::SplitVCursor);
            event->accept();
            return;
            }
      if (controllerContentRect().contains(event->pos())) {
            if (_laneMode == LaneMode::Velocity) {
                  _focusDomain = FocusDomain::Velocity;
                  _selectedPedal = nullptr;
                  beginVelocityGesture(event->pos());
                  }
            else if (_laneMode == LaneMode::Sustain) {
                  _focusDomain = FocusDomain::Sustain;
                  beginPedalGesture(event->pos());
                  }
            else {
                  _focusDomain = FocusDomain::Tempo;
                  _selectedPedal = nullptr;
                  }
            event->accept();
            return;
            }
      if (!noteAreaRect().contains(event->pos()) || !_model)
            return;

      _focusDomain = FocusDomain::Notes;
      if (_selectedPedal) {
            _selectedPedal = nullptr;
            viewport()->update(laneRect());
            }
      _cursorTick = _pressTick;
      const int noteIndex = noteAtPoint(event->pos());
      if (_editTool == EditTool::Erase) {
            if (noteIndex >= 0) {
                  _model->selectEvents({ noteIndex }, KeyEditorModel::SelectionOperation::Replace);
                  _model->deleteSelection();
                  }
            event->accept();
            return;
            }
      if (_editTool == EditTool::Draw) {
            drawNoteAt(event->pos(), false);
            event->accept();
            return;
            }
      if (noteIndex >= 0) {
            selectNoteForClick(noteIndex, event->modifiers());
            if (_model->eventSelected(noteIndex))
                  beginNoteDrag(noteIndex, event->pos());
            }
      else
            beginMarquee(event->pos());
      event->accept();
      }

void KeyEditorView::mouseMoveEvent(QMouseEvent* event)
      {
      const QPoint point = event->pos();
      _lastPos = point;
      _pressModifiers = event->modifiers();
      const int tick = xToTick(point.x());
      const bool pitchDomain = noteAreaRect().contains(point) || keyboardRect().contains(point);
      const int pitch = pitchDomain ? yToPitch(point.y()) : -1;
      if (tick != _cursorTick || pitch != _hoverPitch) {
            _cursorTick = tick;
            const int oldPitch = _hoverPitch;
            _hoverPitch = pitch;
            if (oldPitch >= 0 || _hoverPitch >= 0)
                  viewport()->update(keyboardRect());
            emit cursorChanged(_cursorTick, _hoverPitch);
            }

      switch (_dragMode) {
            case DragMode::Pan:
                  horizontalScrollBar()->setValue(_panStartH - (point.x() - _panStart.x()));
                  verticalScrollBar()->setValue(_panStartV - (point.y() - _panStart.y()));
                  break;
            case DragMode::Marquee: {
                  const QRect oldRegion = previewRegion();
                  _dragThresholdPassed = _dragThresholdPassed
                        || QLineF(_pressPos, point).length() >= QApplication::startDragDistance();
                  const QPoint anchor(_marqueeContentAnchor.x() - horizontalScrollBar()->value(),
                                      _marqueeContentAnchor.y() - verticalScrollBar()->value());
                  _marqueeEndTick = xToTick(point.x());
                  _marqueeEndPitch = yToPitch(point.y());
                  _marquee = QRect(anchor, point).normalized().intersected(noteAreaRect());
                  updateDragRegion(oldRegion);
                  break;
                  }
            case DragMode::MoveNotes:
            case DragMode::ResizeLeft:
            case DragMode::ResizeRight:
                  updateNoteDrag(point, event->modifiers());
                  break;
            case DragMode::DrawNote:
                  _pressModifiers = event->modifiers();
                  drawNoteAt(point, false);
                  break;
            case DragMode::ResizeLane:
                  setControllerHeight(viewport()->height() - point.y());
                  break;
            case DragMode::VelocityHandle:
            case DragMode::VelocityFreehand:
            case DragMode::VelocityLine:
            case DragMode::VelocityBoxUniform:
            case DragMode::VelocityBoxLeft:
            case DragMode::VelocityBoxRight:
                  updateVelocityGesture(point);
                  break;
            case DragMode::PedalCreate:
            case DragMode::PedalMove:
            case DragMode::PedalStart:
            case DragMode::PedalEnd:
                  _pressModifiers = event->modifiers();
                  updatePedalGesture(point);
                  break;
            case DragMode::None:
                  updateCursorForPosition(point);
                  break;
            }
      if (shouldAutoScrollForDrag(point)) {
            if (!_autoScrollTimer->isActive())
                  _autoScrollTimer->start();
            }
      else if (_autoScrollTimer->isActive())
            _autoScrollTimer->stop();
      _lastPos = point;
      event->accept();
      }

void KeyEditorView::mouseReleaseEvent(QMouseEvent* event)
      {
      if (event->button() == Qt::LeftButton && _auditionPitch >= 0
          && keyboardRect().contains(_pressPos)) {
            stopAudition();
            }
      _autoScrollTimer->stop();
      const DragMode finishedMode = _dragMode;
      switch (_dragMode) {
            case DragMode::Marquee:
                  finishMarquee();
                  break;
            case DragMode::MoveNotes:
            case DragMode::ResizeLeft:
            case DragMode::ResizeRight:
                  finishNoteDrag();
                  break;
            case DragMode::DrawNote:
                  drawNoteAt(event->pos(), true);
                  break;
            case DragMode::VelocityHandle:
            case DragMode::VelocityFreehand:
            case DragMode::VelocityLine:
            case DragMode::VelocityBoxUniform:
            case DragMode::VelocityBoxLeft:
            case DragMode::VelocityBoxRight:
                  finishVelocityGesture();
                  break;
            case DragMode::PedalCreate:
            case DragMode::PedalMove:
            case DragMode::PedalStart:
            case DragMode::PedalEnd:
                  finishPedalGesture();
                  break;
            default:
                  break;
            }
      _dragMode = DragMode::None;
      _dragThresholdPassed = false;
      if (event->button() == Qt::LeftButton
          && (finishedMode == DragMode::MoveNotes
              || finishedMode == DragMode::ResizeLeft
              || finishedMode == DragMode::ResizeRight))
            emit notePreviewReleased();
      if (finishedMode == DragMode::Pan || finishedMode == DragMode::ResizeLane)
            setEditTool(_editTool);
      event->accept();
      }

void KeyEditorView::mouseDoubleClickEvent(QMouseEvent* event)
      {
      QAbstractScrollArea::mouseDoubleClickEvent(event);
      }

void KeyEditorView::contextMenuEvent(QContextMenuEvent* event)
      {
      if (!_model)
            return;
      QMenu menu(this);
      auto addNoteTools = [this, &menu]() {
            QActionGroup* group = new QActionGroup(&menu);
            group->setExclusive(true);
            struct ToolEntry {
                  const char* label;
                  EditTool tool;
                  };
            const ToolEntry entries[] = {
                  { QT_TR_NOOP("Select"), EditTool::Select },
                  { QT_TR_NOOP("Draw"), EditTool::Draw },
                  { QT_TR_NOOP("Erase"), EditTool::Erase }
                  };
            for (const ToolEntry& entry : entries) {
                  QAction* action = menu.addAction(tr(entry.label));
                  action->setCheckable(true);
                  action->setChecked(_editTool == entry.tool);
                  group->addAction(action);
                  connect(action, &QAction::triggered, this, [this, entry]() {
                        setEditTool(entry.tool);
                        emit editToolRequested(entry.tool);
                        });
                  }
            };

      auto addLaneTools = [this, &menu]() {
            QActionGroup* toolGroup = new QActionGroup(&menu);
            toolGroup->setExclusive(true);
            struct LaneToolEntry {
                  const char* label;
                  LaneTool tool;
                  };
            const LaneToolEntry velocityEntries[] = {
                  { QT_TR_NOOP("Pointer"), LaneTool::Pointer },
                  { QT_TR_NOOP("Freehand"), LaneTool::Freehand },
                  { QT_TR_NOOP("Line"), LaneTool::Line }
                  };
            const LaneToolEntry sustainEntries[] = {
                  { QT_TR_NOOP("Pointer"), LaneTool::Pointer },
                  { QT_TR_NOOP("Draw span"), LaneTool::Freehand }
                  };
            const LaneToolEntry* entries = _laneMode == LaneMode::Velocity
                                          ? velocityEntries : sustainEntries;
            const int count = _laneMode == LaneMode::Velocity
                            ? int(sizeof(velocityEntries) / sizeof(velocityEntries[0]))
                            : (_laneMode == LaneMode::Sustain
                               ? int(sizeof(sustainEntries) / sizeof(sustainEntries[0])) : 1);
            for (int i = 0; i < count; ++i) {
                  const LaneToolEntry entry = entries[i];
                  QAction* action = menu.addAction(tr(entry.label));
                  action->setCheckable(true);
                  action->setChecked(_laneTool == entry.tool);
                  toolGroup->addAction(action);
                  connect(action, &QAction::triggered, this, [this, entry]() {
                        setLaneTool(entry.tool);
                        emit laneToolRequested(entry.tool);
                        });
                  }
            };

      if (controllerContentRect().contains(event->pos())) {
            _focusDomain = _laneMode == LaneMode::Velocity ? FocusDomain::Velocity
                         : _laneMode == LaneMode::Sustain ? FocusDomain::Sustain
                                                         : FocusDomain::Tempo;
            addLaneTools();
            menu.exec(event->globalPos());
            return;
            }

      _focusDomain = FocusDomain::Notes;
      addNoteTools();
      menu.exec(event->globalPos());
      }

void KeyEditorView::keyPressEvent(QKeyEvent* event)
      {
      if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
            if (!event->isAutoRepeat())
                  emit togglePlaybackRequested();
            event->accept();
            return;
            }
      if (event->key() == Qt::Key_Escape && _dragMode != DragMode::None) {
            cancelActiveGesture();
            event->accept();
            return;
            }
      if (!_model) {
            QAbstractScrollArea::keyPressEvent(event);
            return;
            }
      if (event->matches(QKeySequence::Copy)) {
            _model->copySelection();
            event->accept();
            return;
            }
      if (event->matches(QKeySequence::Cut)) {
            _model->cutSelection();
            event->accept();
            return;
            }
      if (event->matches(QKeySequence::Paste)) {
            _model->pasteAt(snapTick(_cursorTick), _editStaffIdx);
            event->accept();
            return;
            }
      if (event->matches(QKeySequence::SelectAll)) {
            _model->selectAllVisible();
            event->accept();
            return;
            }
      if (event->matches(QKeySequence::Undo)) {
            if (mscore && getAction("undo"))
                  getAction("undo")->trigger();
            event->accept();
            return;
            }
      if (event->matches(QKeySequence::Redo)) {
            if (mscore && getAction("redo"))
                  getAction("redo")->trigger();
            event->accept();
            return;
            }
      if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            if (_focusDomain == FocusDomain::Sustain && _selectedPedal) {
                  _model->deletePedal(_selectedPedal);
                  _selectedPedal = nullptr;
                  }
            else
                  _model->deleteSelection();
            event->accept();
            return;
            }
      const bool ctrl = event->modifiers() & Qt::ControlModifier;
      const bool shift = event->modifiers() & Qt::ShiftModifier;
      const bool alt = event->modifiers() & Qt::AltModifier;
      if (ctrl && event->key() == Qt::Key_D) {
            nudgeSelection(_gridTicks, 0, true);
            event->accept();
            return;
            }
      if (event->key() == Qt::Key_Q && !ctrl && !alt) {
            quantizeSelection();
            event->accept();
            return;
            }
      if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            const int direction = event->key() == Qt::Key_Up ? 1 : -1;
            if (nudgeSelection(0, direction * (shift ? 12 : 1))) {
                  const QList<Note*> selected = _model->selectedNotes();
                  if (!selected.isEmpty() && mscore)
                        mscore->play(selected.front());
                  }
            event->accept();
            return;
            }
      if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            const int direction = event->key() == Qt::Key_Right ? 1 : -1;
            if (alt) {
                  QVector<KeyEditorModel::NoteEdit> edits;
                  for (int index : _model->selectedEventIndexes()) {
                        const KeyEditorModel::NoteBlock& block = _model->notes()[index];
                        edits.append({ block.note, block.startTick,
                                       qMax(block.startTick + 1,
                                            block.endTick + direction * (ctrl ? 1 : _gridTicks)),
                                       block.pitch, block.staffIdx, block.voice,
                                       block.eventIndex });
                        }
                  commitNoteEdits(edits);
                  }
            else
                  nudgeSelection(direction * (ctrl ? 1 : _gridTicks), 0);
            event->accept();
            return;
            }
      QAbstractScrollArea::keyPressEvent(event);
      }

void KeyEditorView::wheelEvent(QWheelEvent* event)
      {
      const int steps = event->angleDelta().y() / 120;
      const Qt::KeyboardModifiers modifiers = event->modifiers();
      if ((modifiers & Qt::ControlModifier) && (modifiers & Qt::ShiftModifier))
            setVerticalZoom(_keyHeight + steps, event->pos().y());
      else if (modifiers & Qt::ControlModifier)
            setHorizontalZoom(_pixelsPerTick * std::pow(1.16, steps), event->pos().x());
      else if (modifiers & Qt::AltModifier)
            setVerticalZoom(_keyHeight + steps, event->pos().y());
      else if (modifiers & Qt::ShiftModifier)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                             - steps * horizontalScrollBar()->singleStep() * 3);
      else
            verticalScrollBar()->setValue(verticalScrollBar()->value()
                                           - steps * verticalScrollBar()->singleStep());
      if (_dragMode == DragMode::Marquee)
            refreshActiveDragAfterScroll();
      event->accept();
      }

void KeyEditorView::keyReleaseEvent(QKeyEvent* event)
      {
      QAbstractScrollArea::keyReleaseEvent(event);
      }

void KeyEditorView::leaveEvent(QEvent* event)
      {
      if (_hoverPitch >= 0) {
            viewport()->update(QRect(0, pitchToY(_hoverPitch), keyboardWidth, _keyHeight));
            _hoverPitch = -1;
            }
      QAbstractScrollArea::leaveEvent(event);
      }

void KeyEditorView::focusOutEvent(QFocusEvent* event)
      {
      if (_dragMode != DragMode::None)
            cancelActiveGesture();
      else {
            stopAudition();
            _autoScrollTimer->stop();
            }
      QAbstractScrollArea::focusOutEvent(event);
      }

void KeyEditorView::hideEvent(QHideEvent* event)
      {
      if (_dragMode != DragMode::None)
            cancelActiveGesture();
      else {
            stopAudition();
            _autoScrollTimer->stop();
            }
      QAbstractScrollArea::hideEvent(event);
      }

} // namespace Ms
