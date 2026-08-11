//=============================================================================
//  MuseScore
//  VST3 settings page
//=============================================================================

#ifndef MS_VST3GUI_H
#define MS_VST3GUI_H

#include "audio/midi/synthesizergui.h"

class QLabel;
class QListWidget;
class QPushButton;
class QLineEdit;
class QTreeWidget;

namespace Ms {

class Vst3Synth;

class Vst3Gui : public SynthesizerGui {
      Q_OBJECT

      QTreeWidget* _plugins { nullptr };
      QListWidget* _directories { nullptr };
      QLineEdit* _filter { nullptr };
      QLabel* _status { nullptr };
      QPushButton* _removeDirectory { nullptr };
      QPushButton* _loadPlugin { nullptr };
      QPushButton* _openEditor { nullptr };
      QPushButton* _useFluidSynth { nullptr };

      Vst3Synth* vstSynth();
      void refresh();
      bool selectedPatch(int& bank, int& program) const;
      int loadSelectedPatch(bool showFailure);
      void selectFirstVisiblePlugin();

   private slots:
      void rescan();
      void addDirectory();
      void removeDirectory();
      void updateButtons();
      void updateFilter(const QString&);
      void loadSelected();
      void openSelectedEditor();
      void useFluidSynth();

   public slots:
      void synthesizerChanged() override;

   public:
      explicit Vst3Gui(Vst3Synth*);
      };

} // namespace Ms

#endif // MS_VST3GUI_H
