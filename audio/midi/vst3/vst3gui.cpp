//=============================================================================
//  MuseScore
//  VST3 settings page
//=============================================================================

#include "vst3gui.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFileDialog>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "vst3synth.h"

namespace Ms {

Vst3Gui::Vst3Gui(Vst3Synth* synthesizer)
   : SynthesizerGui(synthesizer)
      {
      auto* rootLayout = new QVBoxLayout(this);
      rootLayout->setContentsMargins(10, 10, 10, 10);
      rootLayout->setSpacing(8);

      auto* headerLayout = new QHBoxLayout;
      auto* titleLayout = new QVBoxLayout;
      auto* title = new QLabel(tr("VST3 Instruments"), this);
      QFont titleFont = title->font();
      titleFont.setPointSize(titleFont.pointSize() + 2);
      titleFont.setBold(true);
      title->setFont(titleFont);
      auto* explanation = new QLabel(
         tr("Choose an instrument for the current score, then open its native editor from here."), this);
      explanation->setWordWrap(true);
      titleLayout->addWidget(title);
      titleLayout->addWidget(explanation);
      headerLayout->addLayout(titleLayout, 1);

      _status = new QLabel(this);
      _status->setWordWrap(true);
      _status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      _status->setMinimumWidth(190);
      _status->setStyleSheet(QStringLiteral(
         "QLabel { padding: 6px 10px; border: 1px solid palette(mid); border-radius: 3px; }"));
      headerLayout->addWidget(_status);
      rootLayout->addLayout(headerLayout);

      auto* pluginsGroup = new QGroupBox(tr("Available VST3 instruments"), this);
      auto* pluginsLayout = new QVBoxLayout(pluginsGroup);
      auto* filterLayout = new QHBoxLayout;
      auto* filterLabel = new QLabel(tr("Search:"), pluginsGroup);
      _filter = new QLineEdit(pluginsGroup);
      _filter->setClearButtonEnabled(true);
      _filter->setPlaceholderText(tr("Filter by instrument, vendor, or path"));
      filterLayout->addWidget(filterLabel);
      filterLayout->addWidget(_filter, 1);
      pluginsLayout->addLayout(filterLayout);

      _plugins = new QTreeWidget(pluginsGroup);
      _plugins->setColumnCount(4);
      _plugins->setHeaderLabels(QStringList() << tr("Instrument") << tr("Vendor")
                                               << tr("Status") << tr("Location"));
      _plugins->setRootIsDecorated(false);
      _plugins->setAlternatingRowColors(true);
      _plugins->setSelectionMode(QAbstractItemView::SingleSelection);
      _plugins->setUniformRowHeights(true);
      _plugins->setSortingEnabled(true);
      _plugins->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
      _plugins->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
      _plugins->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
      _plugins->header()->setSectionResizeMode(3, QHeaderView::Stretch);
      pluginsLayout->addWidget(_plugins);

      auto* pluginButtonLayout = new QHBoxLayout;
      _loadPlugin = new QPushButton(tr("Load selected"), pluginsGroup);
      _openEditor = new QPushButton(tr("Open editor..."), pluginsGroup);
      _useFluidSynth = new QPushButton(tr("Use FluidSynth"), pluginsGroup);
      auto* rescanButton = new QPushButton(tr("Rescan"), pluginsGroup);
      pluginButtonLayout->addWidget(_loadPlugin);
      pluginButtonLayout->addWidget(_openEditor);
      pluginButtonLayout->addWidget(_useFluidSynth);
      pluginButtonLayout->addStretch();
      pluginButtonLayout->addWidget(rescanButton);
      pluginsLayout->addLayout(pluginButtonLayout);
      rootLayout->addWidget(pluginsGroup, 1);

      auto* directoriesGroup = new QGroupBox(tr("Additional VST3 folders"), this);
      auto* directoriesLayout = new QVBoxLayout(directoriesGroup);
      _directories = new QListWidget(directoriesGroup);
      _directories->setMaximumHeight(95);
      directoriesLayout->addWidget(_directories);

      auto* directoryButtonLayout = new QHBoxLayout;
      auto* addButton = new QPushButton(tr("Add folder..."), directoriesGroup);
      _removeDirectory = new QPushButton(tr("Remove folder"), directoriesGroup);
      directoryButtonLayout->addWidget(addButton);
      directoryButtonLayout->addWidget(_removeDirectory);
      directoryButtonLayout->addStretch();
      directoriesLayout->addLayout(directoryButtonLayout);
      rootLayout->addWidget(directoriesGroup);

      connect(addButton, SIGNAL(clicked()), SLOT(addDirectory()));
      connect(_removeDirectory, SIGNAL(clicked()), SLOT(removeDirectory()));
      connect(rescanButton, SIGNAL(clicked()), SLOT(rescan()));
      connect(_directories, SIGNAL(itemSelectionChanged()), SLOT(updateButtons()));
      connect(_plugins, SIGNAL(itemSelectionChanged()), SLOT(updateButtons()));
      connect(_plugins, SIGNAL(itemDoubleClicked(QTreeWidgetItem*,int)), SLOT(loadSelected()));
      connect(_filter, SIGNAL(textChanged(QString)), SLOT(updateFilter(QString)));
      connect(_loadPlugin, SIGNAL(clicked()), SLOT(loadSelected()));
      connect(_openEditor, SIGNAL(clicked()), SLOT(openSelectedEditor()));
      connect(_useFluidSynth, SIGNAL(clicked()), SLOT(useFluidSynth()));

      refresh();
      }

Vst3Synth* Vst3Gui::vstSynth()
      {
      return static_cast<Vst3Synth*>(synthesizer());
      }

void Vst3Gui::refresh()
      {
      int selectedBank = -1;
      int selectedProgram = -1;
      selectedPatch(selectedBank, selectedProgram);

      _plugins->clear();
      const QList<Vst3PluginDescriptor> descriptors = vstSynth()->descriptors();
      QTreeWidgetItem* itemToSelect = nullptr;
      for (const Vst3PluginDescriptor& descriptor : descriptors) {
            auto* item = new QTreeWidgetItem(_plugins);
            item->setText(0, descriptor.name);
            item->setText(1, descriptor.vendor);
            item->setText(2, vstSynth()->activeChannel(descriptor.bank, descriptor.program) >= 0
                              ? tr("Loaded") : QString());
            item->setText(3, descriptor.path);
            item->setToolTip(3, descriptor.path);
            item->setData(0, Qt::UserRole, descriptor.bank);
            item->setData(0, Qt::UserRole + 1, descriptor.program);
            if (item->text(2) == tr("Loaded")) {
                  QFont font = item->font(0);
                  font.setBold(true);
                  item->setFont(0, font);
                  item->setFont(2, font);
                  }
            if (descriptor.bank == selectedBank && descriptor.program == selectedProgram)
                  itemToSelect = item;
            }

      if (!itemToSelect && _plugins->topLevelItemCount() > 0)
            itemToSelect = _plugins->topLevelItem(0);
      if (itemToSelect)
            _plugins->setCurrentItem(itemToSelect);
      updateFilter(_filter ? _filter->text() : QString());

      _directories->clear();
      _directories->addItems(vstSynth()->customDirectories());

      const QStringList errors = vstSynth()->scanErrors();
      if (descriptors.isEmpty())
            _status->setText(tr("No VST3 instruments were found."));
      else if (errors.isEmpty())
            _status->setText(tr("%n VST3 instrument(s) available.", nullptr, descriptors.size()));
      else
            _status->setText(tr("%1 instrument(s) available; %2 plug-in(s) could not be scanned.")
                             .arg(descriptors.size()).arg(errors.size()));
      _status->setToolTip(errors.join(QLatin1Char('\n')));
      updateButtons();
      }

void Vst3Gui::selectFirstVisiblePlugin()
      {
      QTreeWidgetItem* current = _plugins->currentItem();
      if (current && !current->isHidden())
            return;

      for (int row = 0; row < _plugins->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = _plugins->topLevelItem(row);
            if (!item->isHidden()) {
                  _plugins->setCurrentItem(item);
                  return;
                  }
            }
      _plugins->setCurrentItem(nullptr);
      }

bool Vst3Gui::selectedPatch(int& bank, int& program) const
      {
      QTreeWidgetItem* item = _plugins->currentItem();
      if (!item)
            return false;
      bank = item->data(0, Qt::UserRole).toInt();
      program = item->data(0, Qt::UserRole + 1).toInt();
      return true;
      }

void Vst3Gui::updateFilter(const QString& text)
      {
      const QString needle = text.trimmed();
      for (int row = 0; row < _plugins->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = _plugins->topLevelItem(row);
            QStringList fields;
            fields << item->text(0) << item->text(1) << item->text(2) << item->text(3);
            item->setHidden(!needle.isEmpty()
                            && !fields.join(QLatin1Char(' ')).contains(needle, Qt::CaseInsensitive));
            }
      selectFirstVisiblePlugin();
      updateButtons();
      }

int Vst3Gui::loadSelectedPatch(bool showFailure)
      {
      int bank = 0;
      int program = 0;
      if (!selectedPatch(bank, program))
            return -1;

      emit requestPatchRouting(QStringLiteral("VST3"), bank, program);
      const int channel = vstSynth()->activeChannel(bank, program);
      if (channel < 0 && showFailure) {
            QMessageBox::warning(this, tr("Load VST3 instrument"),
               tr("The instrument could not be assigned. Open a score containing a pitched instrument and try again."));
            }
      refresh();
      return channel;
      }

void Vst3Gui::loadSelected()
      {
      if (loadSelectedPatch(true) >= 0)
            emit valueChanged();
      }

void Vst3Gui::openSelectedEditor()
      {
      int bank = 0;
      int program = 0;
      if (!selectedPatch(bank, program))
            return;

      int channel = vstSynth()->activeChannel(bank, program);
      if (channel < 0) {
            channel = loadSelectedPatch(true);
            if (channel >= 0)
                  emit valueChanged();
            }
      if (channel < 0)
            return;

      if (!vstSynth()->hasEditor(channel) || !vstSynth()->openEditor(channel, this)) {
            QMessageBox::warning(this, tr("VST3 editor"),
                                 tr("This VST3 instrument did not provide a compatible native editor."));
            }
      updateButtons();
      }

void Vst3Gui::useFluidSynth()
      {
      emit requestPatchRouting(QStringLiteral("Fluid"), 0, 0);
      refresh();
      emit valueChanged();
      }

void Vst3Gui::rescan()
      {
      QApplication::setOverrideCursor(Qt::WaitCursor);
      vstSynth()->rescanPlugins();
      QApplication::restoreOverrideCursor();
      refresh();
      emit sfChanged();
      emit valueChanged();
      }

void Vst3Gui::addDirectory()
      {
      const QString directory = QFileDialog::getExistingDirectory(this, tr("Select VST3 folder"));
      if (directory.isEmpty())
            return;
      QApplication::setOverrideCursor(Qt::WaitCursor);
      const bool added = vstSynth()->addCustomDirectory(directory);
      QApplication::restoreOverrideCursor();
      if (!added)
            return;
      refresh();
      emit sfChanged();
      emit valueChanged();
      }

void Vst3Gui::removeDirectory()
      {
      QListWidgetItem* item = _directories->currentItem();
      if (!item)
            return;
      QApplication::setOverrideCursor(Qt::WaitCursor);
      const bool removed = vstSynth()->removeCustomDirectory(item->text());
      QApplication::restoreOverrideCursor();
      if (!removed)
            return;
      refresh();
      emit sfChanged();
      emit valueChanged();
      }

void Vst3Gui::updateButtons()
      {
      _removeDirectory->setEnabled(_directories->currentItem() != nullptr);
      int bank = 0;
      int program = 0;
      const bool hasSelection = selectedPatch(bank, program);
      const int channel = hasSelection ? vstSynth()->activeChannel(bank, program) : -1;
      _loadPlugin->setEnabled(hasSelection);
      _openEditor->setEnabled(hasSelection && (channel < 0 || vstSynth()->hasEditor(channel)));

      bool hasLoadedPlugin = false;
      for (int row = 0; row < _plugins->topLevelItemCount(); ++row) {
            if (!_plugins->topLevelItem(row)->text(2).isEmpty()) {
                  hasLoadedPlugin = true;
                  break;
                  }
            }
      _useFluidSynth->setEnabled(hasLoadedPlugin);
      }

void Vst3Gui::synthesizerChanged()
      {
      refresh();
      }

} // namespace Ms
