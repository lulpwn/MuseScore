//=============================================================================
//  MuseScore
//  Key Editor window
//=============================================================================

#include "pianoroll.h"

#include "keyeditorview.h"
#include "musescore.h"
#include "preferences.h"
#include "seq.h"
#include "shortcut.h"
#include "scoreview.h"

#include "libmscore/instrument.h"
#include "libmscore/measure.h"
#include "libmscore/note.h"
#include "libmscore/part.h"
#include "libmscore/repeatlist.h"
#include "libmscore/score.h"
#include "libmscore/sig.h"
#include "libmscore/staff.h"

#include <QActionGroup>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Ms {

extern MuseScore* mscore;
extern Seq* seq;

static QString editorPitchName(int pitch)
      {
      static const char* names[] = { "C", "C♯", "D", "E♭", "E", "F",
                                     "F♯", "G", "A♭", "A", "B♭", "B" };
      return QString::fromUtf8(names[(pitch % 12 + 12) % 12])
             + QString::number(pitch / 12 - 1);
      }

static QColor editorStaffColor(int ordinal)
      {
      static const QColor colors[] = {
            QColor(74, 144, 226), QColor(238, 139, 61), QColor(68, 170, 117),
            QColor(160, 112, 210), QColor(219, 90, 127), QColor(52, 174, 187)
            };
      return colors[qMax(0, ordinal) % 6];
      }

static QIcon staffSwatch(int ordinal, bool allTracks = false)
      {
      QPixmap pixmap(18, 14);
      pixmap.fill(Qt::transparent);
      QPainter painter(&pixmap);
      painter.setRenderHint(QPainter::Antialiasing, true);
      if (allTracks) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(editorStaffColor(0));
            painter.drawRoundedRect(QRectF(1, 2, 8, 10), 2, 2);
            painter.setBrush(editorStaffColor(1));
            painter.drawRoundedRect(QRectF(9, 2, 8, 10), 2, 2);
            }
      else {
            painter.setPen(QPen(editorStaffColor(ordinal).darker(135), 1));
            painter.setBrush(editorStaffColor(ordinal));
            painter.drawRoundedRect(QRectF(2, 2, 14, 10), 2, 2);
            }
      return QIcon(pixmap);
      }

static QToolButton* makeToolButton(QWidget* parent, const QString& text,
                                   const QString& icon, const QString& tooltip)
      {
      QToolButton* button = new QToolButton(parent);
      button->setText(text);
      if (!icon.isEmpty())
            button->setIcon(QIcon(icon));
      button->setToolTip(tooltip);
      button->setCheckable(true);
      button->setToolButtonStyle(icon.isEmpty() ? Qt::ToolButtonTextOnly
                                                 : Qt::ToolButtonIconOnly);
      return button;
      }

PianorollEditor::PianorollEditor(QWidget* parent)
   : QMainWindow(parent)
      {
      _score = nullptr;
      setObjectName(QStringLiteral("Pianoroll"));
      setWindowTitle(tr("Key Editor"));
      setMinimumSize(760, 480);
      resize(1180, 720);
      setAttribute(Qt::WA_DeleteOnClose, false);

      _model = new KeyEditorModel(this);
      _view = new KeyEditorView(this);
      _view->setModel(_model);
      buildUi();

      QActionGroup* shortcuts = Shortcut::getActionGroupForWidget(MsWidget::PIANO_ROLL_EDITOR);
      shortcuts->setParent(this);
      addActions(shortcuts->actions());
      connect(shortcuts, &QActionGroup::triggered, this, &PianorollEditor::handleAction);

      connect(_model, &KeyEditorModel::selectionChanged,
              this, &PianorollEditor::selectionChanged);
      connect(_view, &KeyEditorView::cursorChanged,
              this, &PianorollEditor::cursorChanged);
      connect(_view, &KeyEditorView::seekRequested,
              this, &PianorollEditor::seekToTick);
      connect(_view, &KeyEditorView::pitchPressed,
              this, &PianorollEditor::pitchPressed);
      connect(_view, &KeyEditorView::pitchReleased,
              this, &PianorollEditor::pitchReleased);
      connect(_view, &KeyEditorView::notePreviewReleased, this, []() {
            if (seq)
                  seq->stopNoteTimer();
            });
      connect(_view, &KeyEditorView::togglePlaybackRequested, this, []() {
            if (QAction* play = getAction("play")) {
                  if (play->isEnabled())
                        play->trigger();
                  }
            });
      if (seq) {
            connect(seq, &Seq::started, this, [this]() {
                  _model->setProjectionUpdatesSuspended(true);
                  });
            connect(seq, &Seq::stopped, this, [this]() {
                  const bool deferred = _rebuildDeferredForPlayback;
                  _rebuildDeferredForPlayback = false;
                  _model->setProjectionUpdatesSuspended(false);
                  if (deferred)
                        scheduleRebuild();
                  });
            }
      connect(_view, &KeyEditorView::noteFocusRequested, this, [this](Note* note) {
            _selectionFromPianoRoll = true;
            focusScoreOnNote(note);
            QTimer::singleShot(0, this, [this]() { _selectionFromPianoRoll = false; });
            });
      connect(_view, &KeyEditorView::pianoRollSelectionStarted, this, [this]() {
            // Marquee selection originates in this editor.  Score selection
            // notifications must not recenter the view that created them.
            _selectionFromPianoRoll = true;
            QTimer::singleShot(0, this, [this]() { _selectionFromPianoRoll = false; });
            });
      connect(_view, &KeyEditorView::editToolRequested, this,
              [this](KeyEditorView::EditTool tool) {
                    if (QAbstractButton* button = _editToolGroup->button(int(tool)))
                          button->setChecked(true);
                    });
      connect(_view, &KeyEditorView::laneModeRequested, this,
              [this](KeyEditorView::LaneMode mode) {
                    const int index = _laneSelector->findData(int(mode));
                    if (index >= 0)
                          _laneSelector->setCurrentIndex(index);
                    });
      connect(_view, &KeyEditorView::laneToolRequested, this,
              [this](KeyEditorView::LaneTool tool) {
                    const int index = _laneToolSelector->findData(int(tool));
                    if (index >= 0)
                          _laneToolSelector->setCurrentIndex(index);
                    });
      readSettings();
      centralWidget()->setEnabled(false);
      }

PianorollEditor::~PianorollEditor()
      {
      detachScore(true);
      }

void PianorollEditor::buildUi()
      {
      QToolBar* editBar = addToolBar(tr("Key Editor Tools"));
      editBar->setObjectName(QStringLiteral("KeyEditorTools"));
      editBar->setMovable(false);
      editBar->setFloatable(false);
      editBar->setIconSize(QSize(18, 18));
      if (mscore) {
            if (qApp->layoutDirection() == Qt::LeftToRight) {
                  editBar->addAction(getAction("undo"));
                  editBar->addAction(getAction("redo"));
                  }
            else {
                  editBar->addAction(getAction("redo"));
                  editBar->addAction(getAction("undo"));
                  }
            editBar->addSeparator();
            editBar->addAction(getAction("rewind"));
            if (QAction* play = getAction("play")) {
                  // ScoreTab narrows this shared action to its own child tree.
                  // Restore its original window scope for the separate Key
                  // Editor window so Space works regardless of focused control.
                  play->setShortcutContext(Qt::WindowShortcut);
                  editBar->addAction(play);
                  }
            editBar->addSeparator();
            }

      _editToolGroup = new QButtonGroup(this);
      _editToolGroup->setExclusive(true);
      QToolButton* select = makeToolButton(editBar, tr("Select"),
            QStringLiteral(":/data/icons/preEdit-select.svg"),
            tr("Select and edit notes. Drag edges to resize; Alt-drag duplicates."));
      QToolButton* draw = makeToolButton(editBar, tr("Draw"),
            QStringLiteral(":/data/icons/preEdit-appendChord.svg"),
            tr("Draw notes. Double-clicking empty space also inserts a note."));
      QToolButton* erase = makeToolButton(editBar, tr("Erase"),
            QStringLiteral(":/data/icons/preEdit-eraseNote.svg"), tr("Delete notes"));
      _editToolGroup->addButton(select, int(KeyEditorView::EditTool::Select));
      _editToolGroup->addButton(draw, int(KeyEditorView::EditTool::Draw));
      _editToolGroup->addButton(erase, int(KeyEditorView::EditTool::Erase));
      select->setChecked(true);
      editBar->addWidget(select);
      editBar->addWidget(draw);
      editBar->addWidget(erase);
      connect(_editToolGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
              this, &PianorollEditor::editToolChanged);

      editBar->addSeparator();
      _snapButton = new QToolButton(editBar);
      _snapButton->setText(tr("Snap"));
      _snapButton->setCheckable(true);
      _snapButton->setChecked(true);
      _snapButton->setToolTip(tr("Snap to the grid. Hold Shift while dragging for free adjustment."));
      editBar->addWidget(_snapButton);
      connect(_snapButton, &QToolButton::toggled, _view, &KeyEditorView::setSnapEnabled);

      _gridSelector = new QComboBox(editBar);
      _gridSelector->setToolTip(tr("Grid and quantize resolution"));
      _gridSelector->addItem(tr("1/4"), 480);
      _gridSelector->addItem(tr("1/8"), 240);
      _gridSelector->addItem(tr("1/8 dotted"), 360);
      _gridSelector->addItem(tr("1/8 triplet"), 160);
      _gridSelector->addItem(tr("1/16"), 120);
      _gridSelector->addItem(tr("1/16 triplet"), 80);
      _gridSelector->addItem(tr("1/32"), 60);
      _gridSelector->addItem(tr("1/64"), 30);
      _gridSelector->setCurrentIndex(4);
      editBar->addWidget(_gridSelector);
      connect(_gridSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
              this, &PianorollEditor::gridChanged);

      QPushButton* quantize = new QPushButton(tr("Quantize"), editBar);
      quantize->setToolTip(tr("Quantize selected note starts to the current grid (Q)"));
      editBar->addWidget(quantize);
      connect(quantize, &QPushButton::clicked, this, [this]() {
            QVector<KeyEditorModel::NoteEdit> edits = selectedEdits();
            const int grid = selectedGridTicks();
            for (KeyEditorModel::NoteEdit& edit : edits) {
                  const int duration = edit.endTick - edit.startTick;
                  edit.startTick = qMax(0, int(std::floor((edit.startTick + grid / 2.0) / grid)) * grid);
                  edit.endTick = edit.startTick + duration;
                  }
            applySelectedEdits(edits);
            });

      _followButton = new QToolButton(editBar);
      _followButton->setText(tr("Follow"));
      _followButton->setCheckable(true);
      _followButton->setChecked(preferences.getBool(PREF_APP_PLAYBACK_FOLLOWSONG));
      _followButton->setToolTip(tr("Keep the playback cursor visible"));
      editBar->addWidget(_followButton);

      QToolBar* contextBar = addToolBar(tr("Key Editor Context"));
      contextBar->setObjectName(QStringLiteral("KeyEditorContext"));
      contextBar->setMovable(false);
      contextBar->setFloatable(false);
      contextBar->addWidget(new QLabel(tr("Track"), contextBar));
      _trackSelector = new QComboBox(contextBar);
      _trackSelector->setMinimumContentsLength(13);
      _trackSelector->setToolTip(tr("Show one staff or all staves in the part"));
      contextBar->addWidget(_trackSelector);
      connect(_trackSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
              this, &PianorollEditor::trackChanged);

      _editTargetLabel = new QLabel(tr("Draw into"), contextBar);
      contextBar->addWidget(_editTargetLabel);
      _editTargetSelector = new QComboBox(contextBar);
      _editTargetSelector->setToolTip(tr("Staff used for new notes and pedal spans in All Tracks mode"));
      contextBar->addWidget(_editTargetSelector);
      connect(_editTargetSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
              this, &PianorollEditor::editTargetChanged);

      _laneSelector = new QComboBox(_view->viewport());
      _laneSelector->addItem(tr("Velocity"), int(KeyEditorView::LaneMode::Velocity));
      _laneSelector->addItem(tr("Sustain (CC64)"), int(KeyEditorView::LaneMode::Sustain));
      _laneSelector->addItem(tr("Tempo Map"), int(KeyEditorView::LaneMode::Tempo));
      _laneSelector->setToolTip(tr("Controller lane"));
      connect(_laneSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
              this, &PianorollEditor::laneChanged);
      _laneToolSelector = new QComboBox(_view->viewport());
      _laneToolSelector->setToolTip(tr("Controller editing tool"));
      connect(_laneToolSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
              this, &PianorollEditor::laneToolChanged);

      _view->setLaneControls(_laneSelector, _laneToolSelector);
      laneChanged(0);

      QWidget* selectionSpacer = new QWidget(contextBar);
      selectionSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
      contextBar->addWidget(selectionSpacer);
      _selectionSummary = new QLabel(tr("No selection"), contextBar);
      _selectionSummary->setMinimumWidth(125);
      contextBar->addWidget(_selectionSummary);

      auto addField = [contextBar](const QString& label, QSpinBox*& field, int minimum, int maximum) {
            contextBar->addWidget(new QLabel(label, contextBar));
            field = new QSpinBox(contextBar);
            field->setRange(minimum, maximum);
            field->setKeyboardTracking(false);
            field->setFixedWidth(88);
            contextBar->addWidget(field);
            };
      addField(tr("Velocity"), _velocityField, 1, 127);
      addField(tr("OnTime"), _onTimeField, -2000, 2000);
      addField(tr("Length"), _eventLengthField, 1, 60000);
      connect(_velocityField, &QSpinBox::editingFinished, this, &PianorollEditor::commitVelocity);
      connect(_onTimeField, &QSpinBox::editingFinished,
              this, &PianorollEditor::commitOnTime);
      connect(_eventLengthField, &QSpinBox::editingFinished,
              this, &PianorollEditor::commitEventLength);
      connect(_velocityField, QOverload<int>::of(&QSpinBox::valueChanged),
              this, [this](int) { _velocityDirty = true; });
      connect(_onTimeField, QOverload<int>::of(&QSpinBox::valueChanged),
              this, [this](int) { _onTimeDirty = true; });
      connect(_eventLengthField, QOverload<int>::of(&QSpinBox::valueChanged),
              this, [this](int) { _eventLengthDirty = true; });

      QWidget* central = new QWidget(this);
      QVBoxLayout* layout = new QVBoxLayout(central);
      layout->setContentsMargins(0, 0, 0, 0);
      layout->setSpacing(0);
      layout->addWidget(_view);
      setCentralWidget(central);

      _cursorSummary = new QLabel(tr("Tick 0  ·  C4"), this);
      statusBar()->addPermanentWidget(_cursorSummary);
      statusBar()->showMessage(tr("Space: play/pause   Shift: free adjustment   Ctrl: constrain/fine nudge   Alt: duplicate   Middle drag: pan"));
      updateSelectionFields();
      }

void PianorollEditor::detachScore(bool removeViewer)
      {
      pitchReleased(-1);
      if (!_score)
            return;
      disconnect(_score, nullptr, this, nullptr);
      if (removeViewer)
            _score->removeViewer(this);
      _score = nullptr;
      }

void PianorollEditor::attachScore(Score* score)
      {
      if (_score == score)
            return;
      detachScore(true);
      _score = score;
      if (!_score)
            return;
      _score->addViewer(this);
      connect(_score, SIGNAL(posChanged(POS,uint)), this, SLOT(scorePositionChanged(POS,uint)));
      connect(_score, SIGNAL(playlistChanged()), this, SLOT(playlistChanged()));
      for (int i = 0; i < 3; ++i) {
            _locators[i].setContext(_score->tempomap(), _score->sigmap());
            _locators[i].setTick(_score->pos(POS(i)).ticks());
            _view->setLocator(i, _locators[i].tick());
            }
      }

void PianorollEditor::setScore(Score* score)
      {
      if (_score == score)
            return;
      _staff = nullptr;
      _partStaves.clear();
      _model->clear();
      centralWidget()->setEnabled(false);
      attachScore(score);
      }

void PianorollEditor::rebuildScopeSelectors(Staff* initialStaff)
      {
      QSignalBlocker trackBlocker(_trackSelector);
      QSignalBlocker targetBlocker(_editTargetSelector);
      _trackSelector->clear();
      _editTargetSelector->clear();
      _partStaves.clear();
      if (!initialStaff || !initialStaff->part())
            return;
      const QList<Staff*>* staves = initialStaff->part()->staves();
      if (staves)
            _partStaves = *staves;
      if (_partStaves.size() > 1)
            _trackSelector->addItem(staffSwatch(0, true), tr("All Tracks"), -1);
      int selectedTarget = 0;
      for (int i = 0; i < _partStaves.size(); ++i) {
            Staff* staff = _partStaves[i];
            QString label;
            if (_partStaves.size() == 2)
                  label = i == 0 ? tr("Staff 1 · Upper") : tr("Staff 2 · Lower");
            else
                  label = tr("Staff %1").arg(i + 1);
            _trackSelector->addItem(staffSwatch(i), label, staff->idx());
            _editTargetSelector->addItem(staffSwatch(i), label, staff->idx());
            if (staff == initialStaff)
                  selectedTarget = i;
            }
      _editTargetSelector->setCurrentIndex(selectedTarget);
      if (_partStaves.size() > 1)
            _trackSelector->setCurrentIndex(0);
      else
            _trackSelector->setCurrentIndex(qMax(0, selectedTarget));
      }

void PianorollEditor::setStaff(Staff* staff)
      {
      if (staff && staff->score() != _score)
            setScore(staff->score());
      _staff = staff;
      rebuildScopeSelectors(staff);
      updateScope();
      centralWidget()->setEnabled(staff != nullptr);
      if (staff) {
            int focusPitch = 60;
            const QList<Note*> selected = _model->selectedNotes();
            if (!selected.isEmpty())
                  focusPitch = selected.front()->pitch();
            _view->ensurePitchVisible(focusPitch, true);
            }
      }

Staff* PianorollEditor::selectedEditStaff() const
      {
      if (_partStaves.isEmpty())
            return nullptr;
      const int index = _editTargetSelector->currentIndex();
      return index >= 0 && index < _partStaves.size() ? _partStaves[index] : _partStaves.front();
      }

void PianorollEditor::updateScope()
      {
      if (!_score || _partStaves.isEmpty()) {
            _model->clear();
            return;
            }
      QList<Staff*> visible;
      Staff* editStaff = selectedEditStaff();
      const bool allTracks = _trackSelector->currentData().toInt() < 0;
      if (allTracks)
            visible = _partStaves;
      else {
            const int staffIdx = _trackSelector->currentData().toInt();
            for (Staff* staff : _partStaves) {
                  if (staff->idx() == staffIdx) {
                        visible.append(staff);
                        editStaff = staff;
                        break;
                        }
                  }
            }
      if (!editStaff)
            editStaff = _partStaves.front();
      _staff = editStaff;
      _editTargetLabel->setVisible(allTracks);
      _editTargetSelector->setVisible(allTracks);
      _editTargetSelector->setEnabled(allTracks);
      _view->setEditStaffIdx(editStaff->idx());
      _model->setProjectionUpdatesSuspended(seq && seq->isPlaying());
      _model->setContext(editStaff, visible);
      updateWindowTitle();
      updateSelectionFields();
      }

void PianorollEditor::updateWindowTitle()
      {
      if (!_score || !_staff) {
            setWindowTitle(tr("Key Editor"));
            return;
            }
      const QString scoreName = _score->masterScore()->fileInfo()->completeBaseName();
      const QString scope = _trackSelector->currentData().toInt() < 0
                          ? tr("All Tracks") : _trackSelector->currentText();
      setWindowTitle(tr("%1 — Key Editor — %2").arg(scoreName, scope));
      }

void PianorollEditor::trackChanged(int)
      {
      if (_trackSelector->currentData().toInt() >= 0) {
            const int staffIdx = _trackSelector->currentData().toInt();
            for (int i = 0; i < _editTargetSelector->count(); ++i) {
                  if (_editTargetSelector->itemData(i).toInt() == staffIdx) {
                        QSignalBlocker blocker(_editTargetSelector);
                        _editTargetSelector->setCurrentIndex(i);
                        break;
                        }
                  }
            }
      updateScope();
      }

void PianorollEditor::editTargetChanged(int)
      {
      if (_trackSelector->currentData().toInt() < 0)
            updateScope();
      }

int PianorollEditor::selectedGridTicks() const
      {
      return qMax(1, _gridSelector->currentData().toInt());
      }

void PianorollEditor::gridChanged(int)
      {
      _view->setGridTicks(selectedGridTicks());
      }

void PianorollEditor::laneChanged(int)
      {
      const KeyEditorView::LaneMode mode = KeyEditorView::LaneMode(_laneSelector->currentData().toInt());
      _view->setLaneMode(mode);
      QSignalBlocker blocker(_laneToolSelector);
      _laneToolSelector->clear();
      _laneToolSelector->addItem(tr("Pointer"), int(KeyEditorView::LaneTool::Pointer));
      if (mode == KeyEditorView::LaneMode::Velocity) {
            _laneToolSelector->addItem(tr("Freehand"), int(KeyEditorView::LaneTool::Freehand));
            _laneToolSelector->addItem(tr("Line"), int(KeyEditorView::LaneTool::Line));
            }
      else if (mode == KeyEditorView::LaneMode::Sustain)
            _laneToolSelector->addItem(tr("Draw span"), int(KeyEditorView::LaneTool::Freehand));
      _laneToolSelector->setCurrentIndex(0);
      _view->setLaneTool(KeyEditorView::LaneTool::Pointer);
      }

void PianorollEditor::laneToolChanged(int)
      {
      if (_laneToolSelector->currentIndex() >= 0)
            _view->setLaneTool(KeyEditorView::LaneTool(_laneToolSelector->currentData().toInt()));
      }

void PianorollEditor::editToolChanged(int tool)
      {
      _view->setEditTool(KeyEditorView::EditTool(tool));
      }

QVector<KeyEditorModel::NoteEdit> PianorollEditor::selectedEdits() const
      {
      QVector<KeyEditorModel::NoteEdit> edits;
      for (int index : _model->selectedEventIndexes()) {
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            edits.append({ block.note, block.startTick, block.endTick, block.pitch,
                           block.staffIdx, block.voice, block.eventIndex });
            }
      return edits;
      }

bool PianorollEditor::applySelectedEdits(const QVector<KeyEditorModel::NoteEdit>& edits)
      {
      return _model->applyPlaybackTimingEdits(edits);
      }

void PianorollEditor::focusScoreOnNote(Note* note)
      {
      if (!note || !mscore || !mscore->currentScoreView())
            return;
      ScoreView* scoreView = mscore->currentScoreView();
      if (scoreView->score() == note->score())
            scoreView->adjustCanvasPosition(note, false);
      }

void PianorollEditor::focusSelectedNoteInPianoRoll()
      {
      if (!_score)
            return;
      Note* note = nullptr;
      Element* selected = _score->selection().element();
      if (selected && selected->isNote())
            note = toNote(selected);
      if (!note) {
            const std::vector<Note*> notes = _score->selection().noteList();
            if (!notes.empty())
                  note = notes.front();
            }
      if (!note)
            return;

      if (!_partStaves.contains(note->staff()))
            setStaff(note->staff());
      const int index = _model->noteIndex(note);
      if (index < 0)
            return;
      const KeyEditorModel::NoteBlock& block = _model->notes()[index];
      _view->ensureTickVisible(block.startTick, true);
      _view->ensurePitchVisible(block.pitch, true);
      }

void PianorollEditor::updateSelectionFields()
      {
      const QVector<KeyEditorModel::NoteEdit> edits = selectedEdits();
      const bool enabled = !edits.isEmpty();
      _velocityField->setEnabled(enabled);
      _onTimeField->setEnabled(enabled);
      _eventLengthField->setEnabled(enabled);
      const QString eventTip = tr("Playback event value for the selected MIDI events");
      _onTimeField->setToolTip(eventTip);
      _eventLengthField->setToolTip(eventTip);
      if (!enabled) {
            _selectionSummary->setText(tr("No selection"));
            _velocityDirty = false;
            _onTimeDirty = _eventLengthDirty = false;
            return;
            }

      QSet<int> staves;
      for (const KeyEditorModel::NoteEdit& edit : edits)
            staves.insert(edit.staffIdx);
      const KeyEditorModel::NoteBlock& firstBlock =
            _model->notes()[_model->selectedEventIndexes().front()];
      const int firstVelocity = firstBlock.velocity;
      bool mixedVelocity = false;
      bool mixedOnTime = false;
      bool mixedLength = false;
      for (int i = 0; i < edits.size(); ++i) {
            const int index = _model->noteIndex(edits[i].source, edits[i].eventIndex);
            if (index < 0)
                  continue;
            const KeyEditorModel::NoteBlock& block = _model->notes()[index];
            mixedVelocity = mixedVelocity || block.velocity != firstVelocity;
            mixedOnTime = mixedOnTime || block.ontime != firstBlock.ontime;
            mixedLength = mixedLength || block.eventLength != firstBlock.eventLength;
            }
      QSignalBlocker velocityBlock(_velocityField);
      QSignalBlocker onTimeBlock(_onTimeField);
      QSignalBlocker eventLengthBlock(_eventLengthField);
      _velocityField->setValue(firstVelocity);
      _onTimeField->setValue(firstBlock.ontime);
      _eventLengthField->setValue(firstBlock.eventLength);
      auto setMixedDisplay = [this](QSpinBox* field, bool mixed) {
            if (QLineEdit* editor = field->findChild<QLineEdit*>()) {
                  editor->setPlaceholderText(mixed ? tr("Mixed") : QString());
                  if (mixed)
                        editor->clear();
                  }
            };
      setMixedDisplay(_velocityField, mixedVelocity);
      setMixedDisplay(_onTimeField, mixedOnTime);
      setMixedDisplay(_eventLengthField, mixedLength);
      _selectionSummary->setText(edits.size() == 1
            ? tr("1 note") : tr("%1 notes · %2 staffs").arg(edits.size()).arg(staves.size()));
      _velocityDirty = false;
      _onTimeDirty = _eventLengthDirty = false;
      }

void PianorollEditor::selectionChanged()
      {
      updateSelectionFields();
      }

void PianorollEditor::cursorChanged(int tick, int pitch)
      {
      QString position = tr("Tick %1").arg(tick);
      if (_score && _score->sigmap()) {
            int bar = 0;
            int beat = 0;
            int remainder = 0;
            _score->sigmap()->tickValues(tick, &bar, &beat, &remainder);
            position = tr("%1.%2.%3").arg(bar + 1).arg(beat + 1).arg(remainder);
            }
      _cursorSummary->setText(pitch >= 0
            ? tr("%1  ·  %2").arg(position).arg(editorPitchName(pitch))
            : position);
      }

void PianorollEditor::commitVelocity()
      {
      if (!_velocityDirty)
            return;
      _velocityDirty = false;
      _model->setSelectionVelocity(_velocityField->value());
      }

void PianorollEditor::commitOnTime()
      {
      if (!_onTimeDirty)
            return;
      _onTimeDirty = false;
      _model->setSelectionEventTiming(_onTimeField->value(), true);
      }

void PianorollEditor::commitEventLength()
      {
      if (!_eventLengthDirty)
            return;
      _eventLengthDirty = false;
      _model->setSelectionEventTiming(_eventLengthField->value(), false);
      }

void PianorollEditor::seekToTick(int tick)
      {
      if (!_score)
            return;
      tick = qBound(0, tick, qMax(0, _model->scoreEndTick()));
      _score->setPos(POS::CURRENT, Fraction::fromTicks(tick));
      if (seq)
            seq->seek(_score->masterScore()->repeatList().tick2utick(tick));
      }

void PianorollEditor::pitchPressed(int pitch, int staffIdx)
      {
      Staff* auditionStaff = nullptr;
      if (_score && staffIdx >= 0 && staffIdx < _score->nstaves())
            auditionStaff = _score->staff(staffIdx);
      if (!auditionStaff)
            auditionStaff = selectedEditStaff();
      if (!auditionStaff)
            auditionStaff = _staff;
      if (!seq || !auditionStaff || !auditionStaff->part()
          || !auditionStaff->part()->instrument())
            return;
      Channel* channel = auditionStaff->part()->instrument()->channel(0);
      if (!channel)
            return;
      if (_auditionChannel >= 0 && _auditionPitch >= 0)
            seq->sendEvent(NPlayEvent(ME_NOTEOFF, _auditionChannel, _auditionPitch, 0));
      _auditionChannel = channel->channel();
      _auditionPitch = qBound(0, pitch, 127);
      // Use the non-timed overload: Seq's timed preview path calls stopNotes(),
      // which resets sustain and pitch bend on every channel (including VSTs).
      seq->startNote(_auditionChannel, _auditionPitch, 80, 0.0);
      }

void PianorollEditor::pitchReleased(int pitch)
      {
      if (!seq || _auditionChannel < 0 || _auditionPitch < 0)
            return;
      if (pitch >= 0 && pitch != _auditionPitch)
            return;
      seq->sendEvent(NPlayEvent(ME_NOTEOFF, _auditionChannel, _auditionPitch, 0));
      _auditionChannel = -1;
      _auditionPitch = -1;
      }

void PianorollEditor::scorePositionChanged(POS position, unsigned tick)
      {
      const int index = int(position);
      if (index < 0 || index > 2)
            return;
      _locators[index].setTick(tick);
      _view->setLocator(index, int(tick));
      if (position == POS::CURRENT)
            _view->setPlaybackTick(int(tick));
      }

void PianorollEditor::heartBeat(Seq* sequence)
      {
      if (!_score || !sequence)
            return;
      unsigned tick = sequence->getCurTick();
      if (_score->masterScore())
            tick = _score->masterScore()->repeatList().utick2tick(tick);
      _view->setPlaybackTick(int(tick));
      if (_followButton->isChecked() && sequence->isPlaying()
          && !_view->interactionActive())
            _view->followPlaybackTick(int(tick));
      }

void PianorollEditor::focusOnPosition(Position* position)
      {
      if (position && position->segment)
            _view->ensureTickVisible(position->segment->tick().ticks(), true);
      }

void PianorollEditor::scheduleRebuild()
      {
      if (_updateScheduled)
            return;
      _updateScheduled = true;
      QTimer::singleShot(0, this, &PianorollEditor::doRebuild);
      }

void PianorollEditor::doRebuild()
      {
      _updateScheduled = false;
      if (seq && seq->isPlaying()) {
            _rebuildDeferredForPlayback = true;
            _model->setProjectionUpdatesSuspended(true);
            return;
            }
      if (_score) {
            if (!_staff || !_score->staves().contains(_staff)) {
                  setStaff(_score->nstaves() ? _score->staff(0) : nullptr);
                  return;
                  }
            const QList<Staff*>* current = _staff->part() ? _staff->part()->staves() : nullptr;
            if (!current || *current != _partStaves) {
                  rebuildScopeSelectors(_staff);
                  updateScope();
                  return;
                  }
            _model->rebuild();
            }
      updateSelectionFields();
      }

void PianorollEditor::dataChanged(const QRectF&)
      {
      scheduleRebuild();
      }

void PianorollEditor::updateAll()
      {
      scheduleRebuild();
      }

void PianorollEditor::playlistChanged()
      {
      scheduleRebuild();
      }

void PianorollEditor::changeSelection(SelState state)
      {
      _model->syncSelection();
      updateSelectionFields();
      if (!_selectionFromPianoRoll && state == SelState::LIST)
            focusSelectedNoteInPianoRoll();
      }

void PianorollEditor::removeScore()
      {
      // Called while Score itself may be destroying its viewer list.  Do not
      // call removeViewer() from here.
      detachScore(false);
      _staff = nullptr;
      _partStaves.clear();
      _model->clear();
      centralWidget()->setEnabled(false);
      }

void PianorollEditor::changeEditElement(Element*)
      {
      scheduleRebuild();
      }

void PianorollEditor::onElementDestruction(Element* element)
      {
      // Score notifications can be followed by a paint event before the
      // coalesced rebuild. Drop only the dying pointer immediately; clearing
      // the entire projection here also resets the viewport during note moves.
      _model->invalidateElement(element);
      scheduleRebuild();
      }

QCursor PianorollEditor::cursor() const
      {
      return _view ? _view->cursor() : QCursor();
      }

void PianorollEditor::setCursor(const QCursor& cursorValue)
      {
      if (_view)
            _view->setCursor(cursorValue);
      }

Element* PianorollEditor::elementNear(QPointF)
      {
      return nullptr;
      }

void PianorollEditor::setEditNoteLength(int)
      {
      // Note length is defined by the horizontal draw gesture.
      }

void PianorollEditor::setEditNoteVoice(int voice)
      {
      _view->setEditVoice(voice);
      }

void PianorollEditor::setEditNoteTool(PianoRollEditTool tool)
      {
      KeyEditorView::EditTool mapped = KeyEditorView::EditTool::Select;
      if (tool == PianoRollEditTool::ADD || tool == PianoRollEditTool::APPEND_NOTE)
            mapped = KeyEditorView::EditTool::Draw;
      else if (tool == PianoRollEditTool::ERASE)
            mapped = KeyEditorView::EditTool::Erase;
      if (QAbstractButton* button = _editToolGroup->button(int(mapped)))
            button->setChecked(true);
      _view->setEditTool(mapped);
      }

void PianorollEditor::setEditNoteDots(int, QToolButton*)
      {
      }

void PianorollEditor::handleAction(QAction* action)
      {
      if (!action)
            return;
      const QString command = action->data().toString();
      if (command == QStringLiteral("zoom-in-horiz-pre"))
            zoom(1, true);
      else if (command == QStringLiteral("zoom-out-horiz-pre"))
            zoom(-1, true);
      else if (command == QStringLiteral("zoom-in-vert-pre"))
            zoom(1, false);
      else if (command == QStringLiteral("zoom-out-vert-pre"))
            zoom(-1, false);
      }

void PianorollEditor::zoom(int amount, bool horizontal)
      {
      if (horizontal) {
            _view->setHorizontalZoom(_view->horizontalZoom() * std::pow(1.16, amount));
            }
      else
            _view->setVerticalZoom(_view->verticalZoom() + amount);
      }

void PianorollEditor::readSettings()
      {
      MuseScore::restoreGeometry(this);
      QSettings settings;
      settings.beginGroup(QStringLiteral("PianoRollKeyEditor"));
      _snapButton->setChecked(settings.value(QStringLiteral("snap"), true).toBool());
      _gridSelector->setCurrentIndex(qBound(0, settings.value(QStringLiteral("grid"), 4).toInt(),
                                             _gridSelector->count() - 1));
      _laneSelector->setCurrentIndex(qBound(0, settings.value(QStringLiteral("lane"), 0).toInt(),
                                             _laneSelector->count() - 1));
      _view->setHorizontalZoom(qBound(18,
            settings.value(QStringLiteral("horizontalZoom"), 120).toInt(), 900) / 1000.0);
      _view->setVerticalZoom(qBound(8,
            settings.value(QStringLiteral("verticalZoom"), 16).toInt(), 34));
      const int defaultControllerHeight = qMax(160, qRound(height() * 0.25));
      _view->setControllerHeight(settings.value(QStringLiteral("controllerHeightV3"),
                                                 defaultControllerHeight).toInt());
      settings.endGroup();
      gridChanged(_gridSelector->currentIndex());
      }

void PianorollEditor::writeSettings()
      {
      MuseScore::saveGeometry(this);
      QSettings settings;
      settings.beginGroup(QStringLiteral("PianoRollKeyEditor"));
      settings.setValue(QStringLiteral("snap"), _snapButton->isChecked());
      settings.setValue(QStringLiteral("grid"), _gridSelector->currentIndex());
      settings.setValue(QStringLiteral("lane"), _laneSelector->currentIndex());
      settings.setValue(QStringLiteral("horizontalZoom"),
                        qRound(_view->horizontalZoom() * 1000.0));
      settings.setValue(QStringLiteral("verticalZoom"), _view->verticalZoom());
      settings.setValue(QStringLiteral("controllerHeight"), _view->controllerHeight());
      settings.setValue(QStringLiteral("controllerHeightV2"), _view->controllerHeight());
      settings.setValue(QStringLiteral("controllerHeightV3"), _view->controllerHeight());
      settings.endGroup();
      }

} // namespace Ms
