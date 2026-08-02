//=============================================================================
//  MuseScore
//  VST3 instrument synthesizer user interface
//=============================================================================

#ifndef __VST3GUI_H__
#define __VST3GUI_H__

#include "audio/midi/synthesizergui.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

namespace Ms {

class Vst3Synth;

class Vst3Gui final : public SynthesizerGui {
      Q_OBJECT

      QComboBox* _plugins { nullptr };
      QComboBox* _audioBuffer { nullptr };
      QCheckBox* _routeToPiano { nullptr };
      QLabel* _pluginPath { nullptr };
      QLabel* _status { nullptr };
      QLabel* _message { nullptr };
      QPushButton* _loadButton { nullptr };
      QPushButton* _editorButton { nullptr };
      QPushButton* _unloadButton { nullptr };
      QPushButton* _scanButton { nullptr };
      QTimer* _serviceTimer { nullptr };

      Vst3Synth* vst3();
      QString selectedPath() const;
      void addPluginPath(const QString& path, bool select);
      void loadCachedPlugins();
      void saveCachedPlugins() const;
      void updateSelectionDetails();
      void updateControls();

   private slots:
      void refreshPlugins();
      void browsePlugin();
      void loadSelectedPlugin();
      void openEditor();
      void unloadPlugin();
      void selectedPluginChanged(int index);
      void routeOptionChanged(bool enabled);
      void audioBufferChanged(int index);
      void servicePlugin();

   public slots:
      void synthesizerChanged() override;

   public:
      explicit Vst3Gui(Synthesizer* synth);
      };

} // namespace Ms

#endif
