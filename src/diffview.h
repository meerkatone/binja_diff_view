// "Diff View" carousel entry: a full graph view (DisassemblyContainer) plus a
// diff toolbar. Two Diff View panes in the same sync group (or split panes)
// form a diffable pair managed by DiffController.
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QtCore/QString>

#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QWidget>

#include "disassemblyview.h"
#include "diffengine.h"
#include "viewframe.h"
#include "viewtype.h"

class FileContext;
class UIContext;

struct DiffWorkspacePane
{
	FileContext* fileContext = nullptr;
	QString dataType;
	uint64_t offset = 0;
	BinaryViewRef data;
};

// Return one current analyzed view per distinct open FileContext, with the
// active file first. Corpus setup and pair setup share this exact view list.
std::vector<DiffWorkspacePane> collectOpenDiffPanes(UIContext* context);

// Run work off the GUI thread behind an application-modal progress dialog, then
// confirm that every pane is still open in the context with the same view.
// Returns false if the user cancelled or a pane changed meanwhile; cancelled
// work still finishes in the background and the caller must ignore its result.
bool runPaneWorkWithProgress(UIContext* context, const std::vector<DiffWorkspacePane>& panes,
	const QString& label, std::function<void()> work);

// Reusable pair drill-down used by both Open Workspace and corpus matrix cells.
void openPairWorkspace(UIContext* context, const DiffWorkspacePane& left,
	const DiffWorkspacePane& right,
	std::optional<bn_graph_diff::PreparedFunctionMatches> preparedMatches = std::nullopt,
	bool requirePreparedSnapshot = false,
	std::optional<BinaryNinja::FunctionViewType> preferredViewType = std::nullopt);

class DiffViewContainer : public QWidget, public ViewContainer
{
	ViewFrame* m_frame;
	BinaryViewRef m_data;
	DisassemblyContainer* m_disasm;
	QLabel* m_status;
	QLabel* m_ilLabel;
	QLabel* m_simLabel;
	QLabel* m_legend;
	QString m_summaryStatus;
	QString m_summaryDetails;
	QPushButton* m_diffButton;
	QPushButton* m_recomputeButton;
	QPushButton* m_clearButton;
	QToolButton* m_prevButton;
	QToolButton* m_nextButton;
	bool m_diffActive = false;
	bool m_busy = false;

  public:
	DiffViewContainer(ViewFrame* frame, BinaryViewRef data);
	~DiffViewContainer() override;

	virtual View* getView() override { return m_disasm->getView(); }
	DisassemblyView* graphView() const { return m_disasm->getDisassembly(); }
	DisassemblyContainer* disassemblyContainer() const { return m_disasm; }
	ViewFrame* frame() const { return m_frame; }
	BinaryViewRef data() const { return m_data; }

	void setStatus(const QString& text);
	// Update the toolbar indicator showing which IL form this pane is displaying.
	void setILLabel(const QString& text);
	// Update the short, always-visible similarity indicator (empty to hide).
	void setSimilarityLabel(const QString& text);
	// Publish one result to the score indicator and compact pane status together.
	// The full status remains available to callers and as the pane's tooltip.
	void setDiffSummary(const QString& status, const QString& details, const QString& similarity);
	void setBusy(bool busy);
	void setDiffActive(bool active);

  private:
	void computeDiff();
	void recomputeDiff();
	void clearDiff();
	void nextChange();
	void prevChange();
};

class DiffViewType : public ViewType
{
  public:
	DiffViewType();
	virtual int getPriority(BinaryViewRef data, const QString& filename) override;
	virtual QWidget* create(BinaryViewRef data, ViewFrame* frame) override;
	static void init();
};
