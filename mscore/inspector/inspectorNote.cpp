//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2011 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENSE.GPL
//=============================================================================

#include "libmscore/score.h"
#include "libmscore/chord.h"
#include "libmscore/note.h"
#include "libmscore/notedot.h"
#include "libmscore/beam.h"
#include "libmscore/stem.h"
#include "libmscore/hook.h"
#include "libmscore/tuplet.h"
#include "libmscore/staff.h"
#include "libmscore/noteevent.h"
#include "libmscore/undo.h"
#include "inspector.h"
#include "inspectorNote.h"

#include <array>

namespace Ms {

//---------------------------------------------------------
//   undoRawVelocityForLinkedNotes
//---------------------------------------------------------

static void undoRawVelocityForLinkedNotes(Note* source, const NoteEventList& events)
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

//---------------------------------------------------------
//   InspectorNote
//---------------------------------------------------------

InspectorNote::InspectorNote(QWidget* parent)
   : InspectorElementBase(parent)
      {
      s.setupUi(addWidget());
      c.setupUi(addWidget());
      acciaccaturaWidget = addWidget();
      a.setupUi(acciaccaturaWidget);
      n.setupUi(addWidget());

      const std::array<int, 9> noteDenominators {{ 4, 6, 8, 12, 16, 24, 32, 48, 64 }};
      for (int denominator : noteDenominators)
            a.noteSpeed->addItem(QString("1/%1").arg(denominator), denominator);

      static const NoteHead::Scheme schemes[] = {
            NoteHead::Scheme::HEAD_AUTO,
            NoteHead::Scheme::HEAD_NORMAL,
            NoteHead::Scheme::HEAD_PITCHNAME,
            NoteHead::Scheme::HEAD_PITCHNAME_NO_ACCIDENTALS,
            NoteHead::Scheme::HEAD_PITCHNAME_GERMAN,
            NoteHead::Scheme::HEAD_PITCHNAME_GERMAN_NO_ACCIDENTALS,
            NoteHead::Scheme::HEAD_SOLFEGE,
            NoteHead::Scheme::HEAD_SOLFEGE_FIXED,
            NoteHead::Scheme::HEAD_SHAPE_NOTE_4,
            NoteHead::Scheme::HEAD_SHAPE_NOTE_7_AIKIN,
            NoteHead::Scheme::HEAD_SHAPE_NOTE_7_FUNK,
            NoteHead::Scheme::HEAD_SHAPE_NOTE_7_WALKER
            };

      static const NoteHead::Group heads[] = {
            NoteHead::Group::HEAD_NORMAL,
            NoteHead::Group::HEAD_CROSS,
            NoteHead::Group::HEAD_PLUS,
            NoteHead::Group::HEAD_XCIRCLE,
            NoteHead::Group::HEAD_WITHX,
            NoteHead::Group::HEAD_TRIANGLE_UP,
            NoteHead::Group::HEAD_TRIANGLE_DOWN,
            NoteHead::Group::HEAD_SLASHED1,
            NoteHead::Group::HEAD_SLASHED2,
            NoteHead::Group::HEAD_DIAMOND,
            NoteHead::Group::HEAD_DIAMOND_OLD,
            NoteHead::Group::HEAD_CIRCLED,
            NoteHead::Group::HEAD_CIRCLED_LARGE,
            NoteHead::Group::HEAD_LARGE_ARROW,

            NoteHead::Group::HEAD_SLASH,
            NoteHead::Group::HEAD_LARGE_DIAMOND,
            NoteHead::Group::HEAD_BREVIS_ALT,

            NoteHead::Group::HEAD_HEAVY_CROSS,
            NoteHead::Group::HEAD_HEAVY_CROSS_HAT,

            NoteHead::Group::HEAD_DO,
            NoteHead::Group::HEAD_RE,
            NoteHead::Group::HEAD_MI,
            NoteHead::Group::HEAD_FA,
            NoteHead::Group::HEAD_SOL,
            NoteHead::Group::HEAD_LA,
            NoteHead::Group::HEAD_TI
            };

      //
      // fix order of noteheads
      //
      for (auto head : heads)
            n.noteHeadGroup->addItem(NoteHead::group2userName(head), int(head));

      for (auto scheme : schemes)
            n.noteHeadScheme->addItem(NoteHead::scheme2userName(scheme), int(scheme));

      // noteHeadType starts at -1: correct values and count one item more (HEAD_AUTO)
      for (int i = 0; i <= int(NoteHead::Type::HEAD_TYPES); ++i) {
            n.noteHeadType->addItem(NoteHead::type2userName(NoteHead::Type(i - 1)));
            n.noteHeadType->setItemData(i, i - 1);
            }

      const std::vector<InspectorItem> iiList = {
            { Pid::SMALL,          0, n.isSmall,       n.resetSmall         },
            { Pid::HEAD_SCHEME,    0, n.noteHeadScheme, n.resetNoteHeadScheme },
            { Pid::HEAD_GROUP,     0, n.noteHeadGroup, n.resetNoteHeadGroup },
            { Pid::HEAD_TYPE,      0, n.noteHeadType,  n.resetNoteHeadType  },
            { Pid::MIRROR_HEAD,    0, n.mirrorHead,    n.resetMirrorHead    },
            { Pid::PLAY,           0, n.play,          n.resetPlay          },
            { Pid::TUNING,         0, n.tuning,        n.resetTuning        },
            { Pid::VELO_TYPE,      0, n.velocityType,  n.resetVelocityType  },
            { Pid::VELO_OFFSET,    0, n.velocity,      n.resetVelocity      },
            { Pid::FIXED,          0, n.fixed,         n.resetFixed         },
            { Pid::FIXED_LINE,     0, n.fixedLine,     n.resetFixedLine     },

            { Pid::OFFSET,         1, c.offset,        c.resetOffset        },
            { Pid::SMALL,          1, c.isSmall,       c.resetSmall         },
            { Pid::NO_STEM,        1, c.stemless,      c.resetStemless      },
            { Pid::STEM_DIRECTION, 1, c.stemDirection, c.resetStemDirection },

            { Pid::PLAY_BEFORE_BEAT, 1, a.playBeforeBeat, a.resetPlayBeforeBeat },
            { Pid::ORNAMENT_NOTE_DENOMINATOR, 1, a.noteSpeed, a.resetNoteSpeed },

            { Pid::LEADING_SPACE,  2, s.leadingSpace,  s.resetLeadingSpace  },
            };
      const std::vector<InspectorPanel> ppList = {
            { s.title, s.panel },
            { c.title, c.panel },
            { a.title, a.panel },
            { n.title, n.panel },
            };
      mapSignals(iiList, ppList);

      connect(n.noteHeadScheme, SIGNAL(currentIndexChanged(int)), SLOT(noteHeadSchemeChanged(int)));

      connect(n.dot1,     SIGNAL(clicked()),     SLOT(dot1Clicked()));
      connect(n.dot2,     SIGNAL(clicked()),     SLOT(dot2Clicked()));
      connect(n.dot3,     SIGNAL(clicked()),     SLOT(dot3Clicked()));
      connect(n.dot4,     SIGNAL(clicked()),     SLOT(dot4Clicked()));
      connect(n.hook,     SIGNAL(clicked()),     SLOT(hookClicked()));
      connect(n.stem,     SIGNAL(clicked()),     SLOT(stemClicked()));
      connect(n.beam,     SIGNAL(clicked()),     SLOT(beamClicked()));
      connect(n.tuplet,   SIGNAL(clicked()),     SLOT(tupletClicked()));
      }

//---------------------------------------------------------
//   setElement
//---------------------------------------------------------

void InspectorNote::setElement()
      {
      Note* note = toNote(inspector->element());

      // Set the appropriate range before the generic Inspector population;
      // otherwise switching away from a raw-velocity note can clamp an Offset
      // value to the previous User range.
      n.velocity->blockSignals(true);
      n.velocity->setRange(hasRawVelocity() ? 1 : -127, 127);
      n.velocity->blockSignals(false);

      int i = note->dots().size();
      n.dot1->setEnabled(i > 0);
      n.dot2->setEnabled(i > 1);
      n.dot3->setEnabled(i > 2);
      n.dot4->setEnabled(i > 3);
      n.stem->setEnabled(note->chord()->stem());
      n.hook->setEnabled(note->chord()->hook());
      n.beam->setEnabled(note->chord()->beam());
      n.tuplet->setEnabled(note->chord()->tuplet());

      InspectorElementBase::setElement();

      //must be placed after InspectorBase::setElement() cause the last one sets resetButton enability
      if (note->staffType()->group() == StaffGroup::STANDARD)
            noteHeadSchemeChanged(n.noteHeadScheme->currentIndex());
      else {
            n.noteHeadScheme->setEnabled(false);
            n.resetNoteHeadScheme->setEnabled(false);
            n.noteHeadGroup->setEnabled(false);
            n.resetNoteHeadGroup->setEnabled(false);
            }

      bool nograce = !note->chord()->isGrace();
      acciaccaturaWidget->setVisible(note->chord()->noteType() == NoteType::ACCIACCATURA);
      s.leadingSpace->setEnabled(nograce);
      s.resetLeadingSpace->setEnabled(nograce && s.leadingSpace->value());

      if (!n.fixed->isChecked())
            n.fixedLine->setEnabled(false);
      if (!n.play->isChecked())
            n.playWidget->setVisible(false);

      updateRawVelocityControls();
      }

//---------------------------------------------------------
//   hasRawVelocity
//---------------------------------------------------------

bool InspectorNote::hasRawVelocity() const
      {
      if (!inspector || !inspector->el())
            return false;
      for (Element* element : *inspector->el()) {
            if (!element || element->type() != ElementType::NOTE)
                  continue;
            for (const NoteEvent& event : toNote(element)->playEvents()) {
                  if (event.velocity() >= 0)
                        return true;
                  }
            }
      return false;
      }

//---------------------------------------------------------
//   updateRawVelocityControls
//---------------------------------------------------------

void InspectorNote::updateRawVelocityControls()
      {
      if (!hasRawVelocity()) {
            n.velocity->setRange(-127, 127);
            n.velocity->setToolTip(QString());
            return;
            }

      int velocity = -1;
      bool mixed = false;
      for (Element* element : *inspector->el()) {
            if (!element || element->type() != ElementType::NOTE)
                  continue;
            const NoteEventList& events = toNote(element)->playEvents();
            for (const NoteEvent& event : events) {
                  if (event.velocity() < 0) {
                        mixed = true;
                        continue;
                        }
                  if (velocity < 0)
                        velocity = event.velocity();
                  else if (velocity != event.velocity())
                        mixed = true;
                  }
            }

      n.velocityType->blockSignals(true);
      n.velocity->blockSignals(true);
      n.velocityType->setCurrentIndex(1); // User: absolute raw MIDI velocity
      n.velocity->setRange(1, 127);
      n.velocity->setValue(qBound(1, velocity, 127));
      n.velocity->setToolTip(mixed
            ? tr("This note contains multiple raw MIDI velocities. Editing this value sets all of them.")
            : tr("Absolute raw MIDI velocity shared with the piano roll."));
      n.velocityType->blockSignals(false);
      n.velocity->blockSignals(false);
      n.resetVelocityType->setEnabled(true);
      n.resetVelocity->setEnabled(true);
      }

//---------------------------------------------------------
//   changeRawVelocity
//---------------------------------------------------------

void InspectorNote::changeRawVelocity(int velocity)
      {
      if (!inspector || !inspector->element())
            return;
      Score* score = inspector->element()->score();
      if (!score)
            return;

      velocity = qBound(1, velocity, 127);
      QHash<Note*, NoteEventList> replacements;
      for (Element* element : *inspector->el()) {
            if (!element || element->type() != ElementType::NOTE)
                  continue;
            Note* note = toNote(element);
            NoteEventList events = note->playEvents();
            bool changed = false;
            for (NoteEvent& event : events) {
                  if (event.velocity() != velocity) {
                        event.setVelocity(velocity);
                        changed = true;
                        }
                  }
            if (changed)
                  replacements.insert(note, events);
            }
      if (replacements.isEmpty())
            return;

      score->startCmd();
      for (auto it = replacements.begin(); it != replacements.end(); ++it)
            undoRawVelocityForLinkedNotes(it.key(), it.value());
      inspector->setInspectorEdit(true);
      score->endCmd();
      inspector->setInspectorEdit(false);
      }

//---------------------------------------------------------
//   clearRawVelocity
//---------------------------------------------------------

void InspectorNote::clearRawVelocity()
      {
      if (!inspector || !inspector->element())
            return;
      Score* score = inspector->element()->score();
      if (!score)
            return;

      QHash<Note*, NoteEventList> replacements;
      for (Element* element : *inspector->el()) {
            if (!element || element->type() != ElementType::NOTE)
                  continue;
            Note* note = toNote(element);
            NoteEventList events = note->playEvents();
            bool changed = false;
            for (NoteEvent& event : events) {
                  if (event.velocity() >= 0) {
                        event.setVelocity(-1);
                        changed = true;
                        }
                  }
            if (changed)
                  replacements.insert(note, events);
            }
      if (replacements.isEmpty())
            return;

      score->startCmd();
      for (auto it = replacements.begin(); it != replacements.end(); ++it)
            undoRawVelocityForLinkedNotes(it.key(), it.value());
      inspector->setInspectorEdit(true);
      score->endCmd();
      inspector->setInspectorEdit(false);
      }

//---------------------------------------------------------
//   valueChanged
//---------------------------------------------------------

void InspectorNote::valueChanged(int idx, bool reset)
      {
      if (idx < 0 || idx >= int(iList.size()))
            return;
      const Pid id = iList[idx].t;
      const bool rawVelocity = hasRawVelocity();

      if (rawVelocity && (id == Pid::VELO_TYPE || id == Pid::VELO_OFFSET)) {
            if (reset || (id == Pid::VELO_TYPE && n.velocityType->currentIndex() == 0))
                  clearRawVelocity();
            else
                  changeRawVelocity(n.velocity->value());
            setElement();
            return;
            }

      InspectorElementBase::valueChanged(idx, reset);
      }

//---------------------------------------------------------
//   noteHeadSchemeChanged
//---------------------------------------------------------

void InspectorNote::noteHeadSchemeChanged(int index)
      {
      Note* note = toNote(inspector->element());
      NoteHead::Scheme scheme = (index == 0 ? note->staffType()->noteHeadScheme() : NoteHead::Scheme(index - 1));
      if (scheme == NoteHead::Scheme::HEAD_NORMAL) {
            n.noteHeadGroup->setEnabled(true);
            n.resetNoteHeadGroup->setEnabled(note->headGroup() != NoteHead::Group::HEAD_NORMAL);
            }
      else {
            n.noteHeadGroup->setEnabled(false);
            n.resetNoteHeadGroup->setEnabled(false);
            }
      }

//---------------------------------------------------------
//   dot1Clicked
//---------------------------------------------------------

void InspectorNote::dot1Clicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      if (note->dots().size() > 0) {
            NoteDot* dot = note->dot(0);
            dot->score()->select(dot);
            dot->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   dot2Clicked
//---------------------------------------------------------

void InspectorNote::dot2Clicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      if (note->dots().size() > 1) {
            NoteDot* dot = note->dot(1);
            dot->score()->select(dot);
            dot->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   dot3Clicked
//---------------------------------------------------------

void InspectorNote::dot3Clicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      if (note->dots().size() > 2) {
            NoteDot* dot = note->dot(2);
            dot->score()->select(dot);
            dot->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   dot4Clicked
//---------------------------------------------------------

void InspectorNote::dot4Clicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      if (note->dots().size() > 3) {
            NoteDot* dot = note->dot(3);
            dot->score()->select(dot);
            dot->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   hookClicked
//---------------------------------------------------------

void InspectorNote::hookClicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      Hook* hook = note->chord()->hook();
      if (hook) {
            note->score()->select(hook);
            note->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   stemClicked
//---------------------------------------------------------

void InspectorNote::stemClicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      Stem* stem = note->chord()->stem();
      if (stem) {
            note->score()->select(stem);
            note->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   beamClicked
//---------------------------------------------------------

void InspectorNote::beamClicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      Beam* beam = note->chord()->beam();
      if (beam) {
            note->score()->select(beam);
            note->score()->update();
            inspector->update();
            }
      }

//---------------------------------------------------------
//   tupletClicked
//---------------------------------------------------------

void InspectorNote::tupletClicked()
      {
      Note* note = toNote(inspector->element());
      if (note == 0)
            return;
      Tuplet* tuplet = note->chord()->tuplet();
      if (tuplet) {
            note->score()->select(tuplet);
            note->score()->update();
            inspector->update();
            }
      }

}

