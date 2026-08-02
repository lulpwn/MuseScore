//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2013 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENSE.GPL
//=============================================================================

#include "inspectorArpeggio.h"
#include "libmscore/arpeggio.h"

#include <array>

namespace Ms {

//---------------------------------------------------------
//   InspectorArpeggio
//---------------------------------------------------------

InspectorArpeggio::InspectorArpeggio(QWidget* parent)
   : InspectorElementBase(parent)
      {
      g.setupUi(addWidget());

      const std::array<int, 9> noteDenominators {{ 4, 6, 8, 12, 16, 24, 32, 48, 64 }};
      for (int denominator : noteDenominators)
            g.noteSpeed->addItem(QString("1/%1").arg(denominator), denominator);
      g.curveType->addItem(tr("Linear"), int(ArpeggioCurveType::LINEAR));
      g.curveType->addItem(tr("Curved (ease-in)"), int(ArpeggioCurveType::CURVED));

      const std::vector<InspectorItem> iiList = {
            { Pid::PLAY,            0,    g.playArpeggio, g.resetPlayArpeggio},
            { Pid::PLAY_BEFORE_BEAT, 0,   g.playBeforeBeat, g.resetPlayBeforeBeat},
            { Pid::ORNAMENT_NOTE_DENOMINATOR, 0, g.noteSpeed, g.resetNoteSpeed},
            { Pid::ARPEGGIO_CURVE_TYPE, 0, g.curveType, g.resetCurveType},
            { Pid::ARPEGGIO_CURVE_AMOUNT, 0, g.curveAmount, g.resetCurveAmount}
            };
      const std::vector<InspectorPanel> ppList = {
            { g.title, g.panel }
            };

      mapSignals(iiList, ppList);
      }

//---------------------------------------------------------
//   postInit
//---------------------------------------------------------

void InspectorArpeggio::postInit()
      {
      const bool curved = g.curveType->currentData().toInt() == int(ArpeggioCurveType::CURVED);
      g.labelCurveAmount->setEnabled(curved);
      g.curveAmount->setEnabled(curved);
      g.resetCurveAmount->setEnabled(curved && g.curveAmount->value() != 75);
      }
}

