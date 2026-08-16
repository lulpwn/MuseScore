//=============================================================================
//  MuseScore
//  Native VST3 editor window
//=============================================================================

#include "vst3editor.h"

#include <algorithm>

#include <QEvent>
#include <QKeyEvent>
#include <QTimer>

#include "vst3plugin.h"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

namespace Ms {

using namespace Steinberg;

uint32 PLUGIN_API Vst3EditorDialog::addRef()
      {
      return FUnknownPrivate::atomicAdd(__funknownRefCount, 1);
      }

uint32 PLUGIN_API Vst3EditorDialog::release()
      {
      if (FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0)
            return 0;
      return __funknownRefCount;
      }

IMPLEMENT_QUERYINTERFACE(Vst3EditorDialog, IPlugFrame, IPlugFrame::iid)

Vst3EditorDialog::Vst3EditorDialog(std::shared_ptr<Vst3Plugin> plugin)
   : QWidget(nullptr), _plugin(std::move(plugin))
      {
      setAttribute(Qt::WA_NativeWindow);
      setAttribute(Qt::WA_DeleteOnClose);
      setAttribute(Qt::WA_QuitOnClose, false);
      setWindowModality(Qt::NonModal);
      setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                     | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint);
      setWindowFlag(Qt::WindowContextHelpButtonHint, false);
      setWindowTitle(_plugin ? _plugin->descriptor().displayName() : tr("VST3 editor"));
      }

Vst3EditorDialog::~Vst3EditorDialog()
      {
      if (_view) {
            _view->setFrame(nullptr);
            _view->removed();
            _view = nullptr;
            }
      if (_plugin)
            _plugin->captureState();
      }

FIDString Vst3EditorDialog::platformType() const
      {
#ifdef Q_OS_WIN
      return kPlatformTypeHWND;
#elif defined(Q_OS_MAC)
      return kPlatformTypeNSView;
#else
      return kPlatformTypeX11EmbedWindowID;
#endif
      }

bool Vst3EditorDialog::attachPluginView()
      {
      if (!_plugin)
            return false;

      _view = _plugin->createView();
      if (!_view || _view->isPlatformTypeSupported(platformType()) != kResultTrue)
            return false;

      createWinId();
      _scaleFactor = std::max(1.0, devicePixelRatioF());
      _view->setFrame(this);
      if (_view->attached(reinterpret_cast<void*>(windowHandle()->winId()), platformType()) != kResultOk) {
            _view->setFrame(nullptr);
            _view = nullptr;
            return false;
            }

      FUnknownPtr<IPlugViewContentScaleSupport> scaleHandler(_view);
      if (scaleHandler)
            scaleHandler->setContentScaleFactor(static_cast<IPlugViewContentScaleSupport::ScaleFactor>(_scaleFactor));

      QTimer::singleShot(0, this, [this]() { sizeToPlugin(); });
      return true;
      }

void Vst3EditorDialog::sizeToPlugin()
      {
      if (!_view)
            return;
      ViewRect rect {};
      if (_view->getSize(&rect) == kResultOk)
            resizeView(_view, &rect);
      }

tresult PLUGIN_API Vst3EditorDialog::resizeView(IPlugView* view, ViewRect* newSize)
      {
      if (!view || !newSize)
            return kInvalidArgument;

      view->checkSizeConstraint(newSize);
      const int width = std::max(1, static_cast<int>(newSize->getWidth() / _scaleFactor));
      const int height = std::max(1, static_cast<int>(newSize->getHeight() / _scaleFactor));
      setFixedSize(width, height);
      view->onSize(newSize);
      return kResultTrue;
      }

bool Vst3EditorDialog::event(QEvent* event)
      {
      if (event && event->spontaneous() && event->type() == QEvent::ShortcutOverride) {
            auto* keyEvent = dynamic_cast<QKeyEvent*>(event);
            if (keyEvent && (keyEvent->key() == 0 || keyEvent->key() == Qt::Key_unknown)) {
                  keyEvent->accept();
                  return true;
                  }
            }
      return QWidget::event(event);
      }

} // namespace Ms
