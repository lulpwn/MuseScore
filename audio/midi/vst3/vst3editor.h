//=============================================================================
//  MuseScore
//  Native VST3 editor window
//=============================================================================

#ifndef MS_VST3EDITOR_H
#define MS_VST3EDITOR_H

#include <memory>

#include <QWidget>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"

namespace Ms {

class Vst3Plugin;

class Vst3EditorDialog : public QWidget, public Steinberg::IPlugFrame {
      DECLARE_FUNKNOWN_METHODS

      std::shared_ptr<Vst3Plugin> _plugin;
      Steinberg::IPtr<Steinberg::IPlugView> _view;
      double _scaleFactor { 1.0 };

      Steinberg::FIDString platformType() const;
      void sizeToPlugin();

   protected:
      bool event(QEvent*) override;

   public:
      explicit Vst3EditorDialog(std::shared_ptr<Vst3Plugin>);
      ~Vst3EditorDialog() override;

      bool attachPluginView();
      Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView*, Steinberg::ViewRect*) override;
      };

} // namespace Ms

#endif // MS_VST3EDITOR_H
