//=============================================================================
//  MuseScore
//  VST3 host types (ported from the MuseScore 4 VST module)
//=============================================================================

#ifndef MS_VST3TYPES_H
#define MS_VST3TYPES_H

#include <QByteArray>
#include <QString>

#include "public.sdk/source/vst/hosting/module.h"

namespace Ms {

struct Vst3PluginDescriptor {
      QString path;
      QString name;
      QString vendor;
      QString classId;
      VST3::Hosting::ClassInfo classInfo;
      int bank = 0;
      int program = 0;

      QString displayName() const
            {
            return vendor.isEmpty() ? QString("VST3 - %1").arg(name)
                                    : QString("VST3 - %1 - %2").arg(vendor, name);
            }
      };

struct Vst3PluginState {
      QByteArray component;
      QByteArray controller;
      };

} // namespace Ms

#endif // MS_VST3TYPES_H
