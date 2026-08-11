//=============================================================================
//  MuseScore
//  VST3 component handler
//=============================================================================

#include "vst3componenthandler.h"

namespace Ms {

IMPLEMENT_FUNKNOWN_METHODS(Vst3AdvancedComponentHandler,
                           Steinberg::Vst::IComponentHandler2,
                           Steinberg::Vst::IComponentHandler2::iid)

IMPLEMENT_REFCOUNT(Vst3ComponentHandler)

Steinberg::tresult PLUGIN_API Vst3ComponentHandler::queryInterface(const Steinberg::TUID interfaceId, void** obj)
      {
      QUERY_INTERFACE(interfaceId, obj, Steinberg::FUnknown::iid, Steinberg::Vst::IComponentHandler)
      QUERY_INTERFACE(interfaceId, obj, Steinberg::Vst::IComponentHandler::iid, Steinberg::Vst::IComponentHandler)

      if (Steinberg::FUnknownPrivate::iidEqual(interfaceId, Steinberg::Vst::IComponentHandler2::iid))
            return _advancedHandler->queryInterface(interfaceId, obj);

      *obj = nullptr;
      return Steinberg::kNoInterface;
      }

Vst3AdvancedComponentHandler::Vst3AdvancedComponentHandler(std::function<void()> dirtyCallback)
   : _dirtyCallback(std::move(dirtyCallback))
      {
      }

Steinberg::tresult Vst3AdvancedComponentHandler::setDirty(Steinberg::TBool state)
      {
      if (state && _dirtyCallback)
            _dirtyCallback();
      return Steinberg::kResultOk;
      }

Steinberg::tresult Vst3AdvancedComponentHandler::requestOpenEditor(Steinberg::FIDString)
      {
      return Steinberg::kResultOk;
      }

Steinberg::tresult Vst3AdvancedComponentHandler::startGroupEdit()
      {
      return Steinberg::kResultOk;
      }

Steinberg::tresult Vst3AdvancedComponentHandler::finishGroupEdit()
      {
      if (_dirtyCallback)
            _dirtyCallback();
      return Steinberg::kResultOk;
      }

Vst3ComponentHandler::Vst3ComponentHandler(
   std::function<void(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue)> editCallback,
   std::function<void()> dirtyCallback)
   : _editCallback(std::move(editCallback)),
     _dirtyCallback(std::move(dirtyCallback)),
     _advancedHandler(new Vst3AdvancedComponentHandler(_dirtyCallback))
      {
      }

Steinberg::tresult Vst3ComponentHandler::beginEdit(Steinberg::Vst::ParamID)
      {
      return Steinberg::kResultOk;
      }

Steinberg::tresult Vst3ComponentHandler::performEdit(
   Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized)
      {
      if (_editCallback)
            _editCallback(id, valueNormalized);
      return Steinberg::kResultOk;
      }

Steinberg::tresult Vst3ComponentHandler::endEdit(Steinberg::Vst::ParamID)
      {
      if (_dirtyCallback)
            _dirtyCallback();
      return Steinberg::kResultOk;
      }

Steinberg::tresult Vst3ComponentHandler::restartComponent(Steinberg::int32)
      {
      if (_dirtyCallback)
            _dirtyCallback();
      return Steinberg::kResultOk;
      }

} // namespace Ms
