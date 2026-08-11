//=============================================================================
//  MuseScore
//  VST3 component handler
//=============================================================================

#ifndef MS_VST3COMPONENTHANDLER_H
#define MS_VST3COMPONENTHANDLER_H

#include <functional>

#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/base/smartpointer.h"

namespace Ms {

class Vst3AdvancedComponentHandler : public Steinberg::Vst::IComponentHandler2 {
      DECLARE_FUNKNOWN_METHODS

      std::function<void()> _dirtyCallback;

   public:
      explicit Vst3AdvancedComponentHandler(std::function<void()> dirtyCallback);

      Steinberg::tresult PLUGIN_API setDirty(Steinberg::TBool state) override;
      Steinberg::tresult PLUGIN_API requestOpenEditor(Steinberg::FIDString name) override;
      Steinberg::tresult PLUGIN_API startGroupEdit() override;
      Steinberg::tresult PLUGIN_API finishGroupEdit() override;
      };

class Vst3ComponentHandler : public Steinberg::Vst::IComponentHandler {
      DECLARE_FUNKNOWN_METHODS

      std::function<void(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue)> _editCallback;
      std::function<void()> _dirtyCallback;
      Steinberg::FUnknownPtr<Vst3AdvancedComponentHandler> _advancedHandler;

   public:
      Vst3ComponentHandler(
         std::function<void(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue)> editCallback,
         std::function<void()> dirtyCallback);

      Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
      Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                                Steinberg::Vst::ParamValue valueNormalized) override;
      Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
      Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;
      };

} // namespace Ms

#endif // MS_VST3COMPONENTHANDLER_H
