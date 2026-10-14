#include "diffview.h"

#include <algorithm>
#include <map>
#include <optional>
#include <tuple>

#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QFutureWatcher>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "diffcontroller.h"
#include "corpusview.h"
#include "diffresultspanel.h"
#include "diffrenderlayer.h"
#include "patchsignature.h"
#include "filecontext.h"
#include "uitypes.h"
#include "uicontext.h"

using namespace BinaryNinja;

namespace {

class SideBySideDiffTab;
static std::map<ViewFrame*, SideBySideDiffTab*> g_diffWorkspaces;

static void configureCompactCombo(QComboBox* combo, int minimumCharacters)
{
	combo->setFrame(false);
	combo->setMinimumContentsLength(minimumCharacters);
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	combo->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
}

class SideBySideDiffTab : public QWidget
{
  public:
	SideBySideDiffTab(FileContext* left, const QString& leftType, uint64_t leftOffset,
	    FileContext* right, const QString& rightType, uint64_t rightOffset,
	    std::optional<bn_graph_diff::PreparedFunctionMatches> preparedMatches,
	    bool requirePreparedSnapshot,
	    std::optional<FunctionViewType> preferredViewType,
	    QWidget* parent = nullptr) :
	    QWidget(parent), m_leftDiffType(leftType), m_rightDiffType(rightType),
	    m_preparedMatches(std::move(preparedMatches)),
	    m_preferredViewType(std::move(preferredViewType)),
	    m_requirePreparedSnapshot(requirePreparedSnapshot)
	{
		QSplitter* graphSplitter = new QSplitter(Qt::Horizontal, this);
		// Host real ViewFrames (not frameless DiffViewContainers) so each pane has
		// a proper owning ViewFrame. This makes IL switching, the command palette,
		// and sync groups work natively instead of dereferencing a null frame. The
		// type string must be the combined "BinjaDiffView:<dataType>" form so the frame
		// resolves the analyzed data view rather than falling back to raw Hex.
		m_left = new ViewFrame(graphSplitter, left, leftType);
		m_right = new ViewFrame(graphSplitter, right, rightType);
		g_diffWorkspaces[m_left] = this;
		g_diffWorkspaces[m_right] = this;
		DiffController::instance().pairFrames(m_left, m_right);
		graphSplitter->addWidget(m_left);
		graphSplitter->addWidget(m_right);
		graphSplitter->setSizes({1, 1});
		// Carry the analyst's current locations into the new diff frames so the
		// cold-start preview compares the pair they were already looking at.
		m_left->navigate(leftType, leftOffset);
		m_right->navigate(rightType, rightOffset);

		m_graphSection = new QWidget(this);
		QVBoxLayout* graphLayout = new QVBoxLayout(m_graphSection);
		graphLayout->setContentsMargins(0, 0, 0, 0);
		graphLayout->setSpacing(0);
		QWidget* graphHeader = new QWidget(m_graphSection);
		QHBoxLayout* graphHeaderLayout = new QHBoxLayout(graphHeader);
		graphHeaderLayout->setContentsMargins(8, 2, 8, 2);
		graphHeaderLayout->addWidget(new QLabel("View", graphHeader));
		m_viewMode = new QComboBox(graphHeader);
		m_viewMode->addItems({"Graph diff", "Linear"});
		configureCompactCombo(m_viewMode, 10);
		m_viewMode->setToolTip("Switch both panes together; Graph diff restores the selected match overlay");
		graphHeaderLayout->addWidget(m_viewMode);
		graphHeaderLayout->addSpacing(12);
		graphHeaderLayout->addWidget(new QLabel("Layout", graphHeader));
		m_graphMode = new QComboBox(graphHeader);
		m_graphMode->addItems({"Show panes", "Hide panes"});
		configureCompactCombo(m_graphMode, 10);
		m_graphMode->setToolTip("Show both comparison panes or only the function table");
		graphHeaderLayout->addWidget(m_graphMode);
		graphHeaderLayout->addSpacing(12);
		graphHeaderLayout->addWidget(new QLabel("IL", graphHeader));
		m_ilMode = new QComboBox(graphHeader);
		m_ilMode->addItem("Loading…");
		configureCompactCombo(m_ilMode, 14);
		m_ilMode->setEnabled(false);
		m_ilMode->setToolTip("Switch both synchronized panes to the selected IL form");
		graphHeaderLayout->addWidget(m_ilMode);
		graphHeaderLayout->addStretch(1);
		graphLayout->addWidget(graphHeader);
		m_graphHeader = graphHeader;

		m_graphContent = graphSplitter;
		graphLayout->addWidget(m_graphContent, 1);

		QVBoxLayout* layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);

		// Keep the restore control in a small bottom section that remains visible
		// when the result table is hidden.
		QWidget* resultsSection = new QWidget(this);
		QVBoxLayout* resultsLayout = new QVBoxLayout(resultsSection);
		resultsLayout->setContentsMargins(0, 0, 0, 0);
		resultsLayout->setSpacing(0);
		m_resultsToggle = new QToolButton(resultsSection);
		m_resultsToggle->setCheckable(true);
		m_resultsToggle->setChecked(true);
		m_resultsToggle->setText("▾ Hide function matches");
		m_resultsToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
		m_resultsToggle->setAutoRaise(true);
		m_resultsToggle->setToolTip("Show or hide the whole-binary function match navigator");
		resultsLayout->addWidget(m_resultsToggle);

		m_results = new DiffResultsPanel(m_left, m_right, resultsSection);
		resultsLayout->addWidget(m_results, 1);
		m_workspace = new QSplitter(Qt::Vertical, this);
		m_workspace->addWidget(m_graphSection);
		m_workspace->addWidget(resultsSection);
		m_workspace->setSizes({700, 300});
		m_workspace->setStretchFactor(0, 1);
		m_workspace->setStretchFactor(1, 0);
		m_workspace->setCollapsible(0, false);
		m_workspace->setCollapsible(1, false);
		layout->addWidget(m_workspace, 1);

		connect(m_graphMode, &QComboBox::currentIndexChanged, this,
		    [this](int mode) { setGraphMode(mode); });
		connect(m_viewMode, &QComboBox::currentIndexChanged, this,
		    [this](int mode) { setPresentationMode(mode); });
		connect(m_ilMode, &QComboBox::currentIndexChanged, this,
		    [this](int index) { setILMode(index); });
		connect(&DiffController::instance(), &DiffController::ilViewTypeUpdated, this,
		    [this](ViewFrame* frame, const QString&) {
			if (frame == m_left || frame == m_right)
				syncILChoice();
		    });
		connect(m_workspace, &QSplitter::splitterMoved, this,
		    [this](int, int) {
			if (m_graphMode->currentIndex() == 0 && !m_workspace->sizes().empty())
				m_fullGraphHeight = m_workspace->sizes().front();
		    });

		connect(m_resultsToggle, &QToolButton::toggled, this,
		    [this](bool visible) {
			m_results->setVisible(visible);
			m_resultsToggle->setText(
			    visible ? "▾ Hide function matches" : "▸ Show function matches");
			resizeWorkspace();
		    });

		// ViewFrames register their DiffViewContainers during event processing.
		// Finish configuring the native panes before consulting or starting the
		// whole-binary match. In particular, startup must not change IL after a
		// worker has captured its analysis epochs.
		QTimer::singleShot(0, m_results, [this]() {
			BinaryViewRef leftView = m_left->getCurrentBinaryView();
			BinaryViewRef rightView = m_right->getCurrentBinaryView();
			bool preparedCurrent = m_preparedMatches
			    && bn_graph_diff::DiffEngine::instance().preparedFunctionMatchesCurrent(
			        leftView, rightView, *m_preparedMatches);
			initializeILChoices();
			if (m_preferredViewType)
			{
				auto selected = std::find(
				    m_ilTypes.begin(), m_ilTypes.end(), *m_preferredViewType);
				if (selected != m_ilTypes.end())
					setILMode(static_cast<int>(std::distance(m_ilTypes.begin(), selected)));
			}
			if (m_requirePreparedSnapshot && !preparedCurrent)
			{
				m_results->showInitialError(
				    "The corpus pair changed while its drill-down workspace was opening.");
				return;
			}
			m_preparedMatches.reset();
			m_results->beginInitialMatch();
		});
	}

	~SideBySideDiffTab() override
	{
		g_diffWorkspaces.erase(m_left);
		g_diffWorkspaces.erase(m_right);
	}

	void handleFrameViewChange(const QString& type)
	{
		if (m_switchingViews)
			return;
		QString baseType = type.section(':', 0, 0);
		if (baseType != "Linear" && baseType != "Graph" && baseType != "BinjaDiffView")
			return;
		int mode = baseType == "Linear" ? 1 : 0;
		m_viewMode->blockSignals(true);
		m_viewMode->setCurrentIndex(mode);
		m_viewMode->blockSignals(false);
		setPresentationMode(mode);
	}

  private:
	View* activeView(ViewFrame* frame) const
	{
		return frame ? frame->getCurrentViewInterface() : nullptr;
	}

	void initializeILChoices()
	{
		BinaryViewRef leftView = m_left->getCurrentBinaryView();
		BinaryViewRef rightView = m_right->getCurrentBinaryView();
		if (!leftView || !rightView)
			return;
		std::vector<FunctionViewType> candidates {
		    FunctionViewType(NormalFunctionGraph),
		    FunctionViewType(LiftedILFunctionGraph),
		    FunctionViewType(LowLevelILFunctionGraph),
		    FunctionViewType(LowLevelILSSAFormFunctionGraph),
		    FunctionViewType(MediumLevelILFunctionGraph),
		    FunctionViewType(MediumLevelILSSAFormFunctionGraph),
		    FunctionViewType(MappedMediumLevelILFunctionGraph),
		    FunctionViewType(MappedMediumLevelILSSAFormFunctionGraph),
		    FunctionViewType(HighLevelILFunctionGraph),
		    FunctionViewType(HighLevelILSSAFormFunctionGraph)};
		for (const auto& language : LanguageRepresentationFunctionType::GetTypes())
			if (language)
				candidates.emplace_back(language->GetName());

		m_ilTypes.clear();
		m_ilMode->blockSignals(true);
		m_ilMode->clear();
		for (const auto& candidate : candidates)
		{
			if (!candidate.IsValidForView(leftView) || !candidate.IsValidForView(rightView))
				continue;
			if (std::find(m_ilTypes.begin(), m_ilTypes.end(), candidate) != m_ilTypes.end())
				continue;
			m_ilTypes.push_back(candidate);
			m_ilMode->addItem(ilDisplayName(candidate));
		}
		m_ilMode->setEnabled(!m_ilTypes.empty());
		m_ilMode->blockSignals(false);

		// Preserve the graph's existing IL form. Automatically selecting HLIL here
		// can start lazy IL generation even for an already-saved BNDB, invalidating
		// the match which startup just launched and making opening look like a full
		// reanalysis. The user can still select HLIL explicitly.
		syncILChoice();
	}

	void syncILChoice()
	{
		View* view = activeView(m_left);
		if (!view || m_ilTypes.empty())
			return;
		auto current = std::find(m_ilTypes.begin(), m_ilTypes.end(), view->getILViewType());
		if (current == m_ilTypes.end())
			return;
		m_ilMode->blockSignals(true);
		m_ilMode->setCurrentIndex(static_cast<int>(std::distance(m_ilTypes.begin(), current)));
		m_ilMode->blockSignals(false);
	}

	void setILMode(int index)
	{
		if (index < 0 || static_cast<size_t>(index) >= m_ilTypes.size())
			return;
		View* leftView = activeView(m_left);
		if (!leftView)
			return;
		const FunctionViewType& type = m_ilTypes[static_cast<size_t>(index)];
		if (leftView->getILViewType() != type)
		{
			leftView->setILViewType(type);
			leftView->refreshContents();
		}
		// The controller mirrors normal IL notifications. Set the partner here as
		// well so the selector remains reliable during a just-created Linear view,
		// before that view has installed all of its notification plumbing.
		View* rightView = activeView(m_right);
		if (rightView && rightView->getILViewType() != type)
		{
			rightView->setILViewType(type);
			rightView->refreshContents();
		}
	}

	void setPresentationMode(int mode)
	{
		if (mode < 0 || mode > 1 || m_switchingViews)
			return;
		m_switchingViews = true;
		uint64_t leftOffset = m_left->getCurrentOffset();
		uint64_t rightOffset = m_right->getCurrentOffset();
		if (mode == 1)
		{
			FunctionViewType previousType = activeView(m_left)
			    ? activeView(m_left)->getILViewType() : InvalidILViewType;
			DiffController::instance().setGraphPresentation(m_left, m_right, false);
			QString leftType = QString("Linear:%1").arg(m_left->getCurrentDataType());
			QString rightType = QString("Linear:%1").arg(m_right->getCurrentDataType());
			m_left->navigate(leftType, leftOffset);
			m_right->navigate(rightType, rightOffset);
			m_results->setLinearMode(true);
			m_ilMode->setEnabled(!m_ilTypes.empty());
			if (previousType != InvalidILViewType)
			{
				auto selected = std::find(m_ilTypes.begin(), m_ilTypes.end(), previousType);
				if (selected != m_ilTypes.end())
					setILMode(static_cast<int>(std::distance(m_ilTypes.begin(), selected)));
			}
			syncILChoice();
		}
		else
		{
			// The selected pair is restored after the switch. Open both graphs at
			// that pair as well: a pane whose second navigation races the switch
			// otherwise stays at its own Linear cursor, out of step with its partner.
			if (auto selected = m_results->selectedMatchAddresses())
				std::tie(leftOffset, rightOffset) = *selected;
			m_left->navigate(m_leftDiffType, leftOffset);
			m_right->navigate(m_rightDiffType, rightOffset);
			DiffController::instance().setGraphPresentation(m_left, m_right, true);
			m_results->setLinearMode(false);
			m_ilMode->setEnabled(!m_ilTypes.empty());
			QTimer::singleShot(0, m_results, [this]() {
				m_results->restoreSelectedGraphDiff();
				syncILChoice();
			});
		}
		m_switchingViews = false;
	}

	void setGraphMode(int mode)
	{
		m_workspace->setOrientation(Qt::Vertical);
		m_graphSection->setMaximumWidth(QWIDGETSIZE_MAX);
		if (mode == 0)
		{
			m_graphSection->setMaximumHeight(QWIDGETSIZE_MAX);
			m_graphContent->show();
		}
		else
		{
			m_graphContent->hide();
			m_graphSection->setMaximumHeight(m_graphHeader->sizeHint().height());
		}
		resizeWorkspace();
	}

	void resizeWorkspace()
	{
		if (!m_workspace)
			return;
		int resultsMinimum = std::max(1, m_resultsToggle->sizeHint().height());
		int mode = m_graphMode->currentIndex();
		int totalHeight = std::max(1, m_workspace->height());
		int graphHeight;
		if (mode == 0)
			graphHeight = m_results->isVisible()
			    ? std::clamp(m_fullGraphHeight, 200, std::max(200, totalHeight - 200))
			    : std::max(1, totalHeight - resultsMinimum);
		else
			graphHeight = std::min(m_graphHeader->sizeHint().height(), totalHeight);
		m_workspace->setSizes({graphHeight, std::max(1, totalHeight - graphHeight)});
	}

	ViewFrame* m_left;
	ViewFrame* m_right;
	QWidget* m_graphSection;
	QWidget* m_graphHeader;
	QWidget* m_graphContent;
	QComboBox* m_viewMode;
	QComboBox* m_graphMode;
	QComboBox* m_ilMode;
	DiffResultsPanel* m_results;
	QSplitter* m_workspace;
	QToolButton* m_resultsToggle;
	std::vector<FunctionViewType> m_ilTypes;
	QString m_leftDiffType;
	QString m_rightDiffType;
	std::optional<bn_graph_diff::PreparedFunctionMatches> m_preparedMatches;
	std::optional<FunctionViewType> m_preferredViewType;
	bool m_requirePreparedSnapshot = false;
	int m_fullGraphHeight = 700;
	bool m_switchingViews = false;
};

static QString fileContextLabel(FileContext* fc)
{
	if (!fc)
		return "unknown";
	QString filename = fc->getFilename();
	return filename.isEmpty() ? "unknown" : QFileInfo(filename).fileName();
}

static void openLoadedBinariesSideBySide()
{
	UIContext* ctx = UIContext::activeContext();
	if (!ctx)
		return;

	std::vector<DiffWorkspacePane> panes = collectOpenDiffPanes(ctx);

	if (panes.size() < 2)
	{
		QMessageBox::warning(nullptr, "Diff View",
		    "Open two different binaries first, then run this action again.");
		return;
	}
	DiffWorkspacePane left = panes.front();
	DiffWorkspacePane right = panes[1];
	if (panes.size() > 2)
	{
		QStringList choices;
		for (size_t i = 1; i < panes.size(); i++)
			choices.push_back(QString("%1. %2").arg(i).arg(fileContextLabel(panes[i].fileContext)));
		bool accepted = false;
		QString selected = QInputDialog::getItem(nullptr, "Diff View",
		    QString("Compare %1 with:").arg(fileContextLabel(left.fileContext)), choices, 0, false, &accepted);
		if (!accepted)
			return;
		int selectedIndex = choices.indexOf(selected);
		if (selectedIndex < 0)
			return;
		right = panes[static_cast<size_t>(selectedIndex) + 1];
	}

	// Probe the durable pair artifact while the original analyzed views are
	// still current and idle. Workspace construction must consume this result;
	// it must not be the operation that decides whether matching is necessary.
	// The probe hashes both saved databases on first use, so it runs off the GUI
	// thread.
	auto preparedMatches =
	    std::make_shared<std::optional<bn_graph_diff::PreparedFunctionMatches>>();
	BinaryViewRef leftData = left.data, rightData = right.data;
	if (!runPaneWorkWithProgress(ctx, {left, right}, "Checking cached function matches…",
	        [preparedMatches, leftData, rightData]() {
		        *preparedMatches = bn_graph_diff::DiffEngine::instance()
		            .prepareCachedFunctionMatches(leftData, rightData);
	        }))
		return;
	openPairWorkspace(ctx, left, right, std::move(*preparedMatches));
}

}  // namespace

bool runPaneWorkWithProgress(UIContext* context, const std::vector<DiffWorkspacePane>& panes,
	const QString& label, std::function<void()> work)
{
	QProgressDialog progress(label, "Cancel", 0, 0);
	progress.setWindowTitle("Binja Diff");
	progress.setWindowModality(Qt::ApplicationModal);
	progress.setMinimumDuration(300);
	QFutureWatcher<void> watcher;
	QEventLoop loop;
	QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
	QObject::connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
	watcher.setFuture(QtConcurrent::run([work = std::move(work)]() {
		try
		{
			work();
		}
		catch (const std::exception& exception)
		{
			LogWarn("binja_diff_view: background preparation failed: %s", exception.what());
		}
	}));
	if (!watcher.isFinished())
		loop.exec();
	if (!watcher.isFinished() || progress.wasCanceled())
		return false;
	// The dialog appears only after a delay, so tabs could close before it did.
	const auto contexts = UIContext::allContexts();
	if (!contexts.count(context))
		return false;
	const auto current = collectOpenDiffPanes(context);
	for (const auto& pane : panes)
		if (std::none_of(current.begin(), current.end(), [&pane](const DiffWorkspacePane& open) {
			    return open.fileContext == pane.fileContext && open.data == pane.data;
		    }))
			return false;
	return true;
}

std::vector<DiffWorkspacePane> collectOpenDiffPanes(UIContext* context)
{
	std::vector<DiffWorkspacePane> panes;
	if (!context)
		return panes;
	auto addFrame = [&panes](ViewFrame* frame) {
		if (!frame || !frame->getFileContext())
			return;
		for (const auto& pane : panes)
			if (pane.fileContext == frame->getFileContext())
				return;
		DiffWorkspacePane pane;
		pane.fileContext = frame->getFileContext();
		pane.dataType = frame->getCurrentDataType();
		pane.offset = frame->getCurrentOffset();
		pane.data = frame->getCurrentBinaryView();
		panes.push_back(std::move(pane));
	};

	addFrame(context->getCurrentViewFrame());
	for (QWidget* tab : context->getTabs())
		for (ViewFrame* frame : context->getAllViewFramesForTab(tab))
			addFrame(frame);
	return panes;
}

void openPairWorkspace(UIContext* context, const DiffWorkspacePane& left,
	const DiffWorkspacePane& right,
	std::optional<bn_graph_diff::PreparedFunctionMatches> preparedMatches,
	bool requirePreparedSnapshot,
	std::optional<FunctionViewType> preferredViewType)
{
	if (!context || !left.fileContext || !right.fileContext || !left.data || !right.data)
		return;
	auto diffType = [](const QString& dataType) {
		return dataType.isEmpty() ? QString("BinjaDiffView")
		                          : QString("BinjaDiffView:%1").arg(dataType);
	};
	auto* tab = new SideBySideDiffTab(left.fileContext, diffType(left.dataType), left.offset,
	    right.fileContext, diffType(right.dataType), right.offset,
	    std::move(preparedMatches), requirePreparedSnapshot, std::move(preferredViewType));
	QString name = QString("Diff: %1 | %2")
	    .arg(fileContextLabel(left.fileContext), fileContextLabel(right.fileContext));
	context->createTabForWidget(name, tab);
}

DiffViewContainer::DiffViewContainer(ViewFrame* frame, BinaryViewRef data) :
    QWidget(nullptr), m_frame(frame), m_data(data)
{
	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	QWidget* toolbar = new QWidget(this);
	QVBoxLayout* toolbarLayout = new QVBoxLayout(toolbar);
	toolbarLayout->setContentsMargins(8, 4, 8, 4);
	toolbarLayout->setSpacing(2);
	QHBoxLayout* actionLayout = new QHBoxLayout();
	actionLayout->setContentsMargins(0, 0, 0, 0);
	QHBoxLayout* detailLayout = new QHBoxLayout();
	detailLayout->setContentsMargins(0, 0, 0, 0);

	m_diffButton = new QPushButton("Diff", toolbar);
	m_diffButton->setToolTip("Diff with the paired pane");
	connect(m_diffButton, &QPushButton::clicked, this, &DiffViewContainer::computeDiff);
	actionLayout->addWidget(m_diffButton);

	m_recomputeButton = new QPushButton("Rematch", toolbar);
	m_recomputeButton->setToolTip("Discard cached whole-binary matches and recompute");
	connect(m_recomputeButton, &QPushButton::clicked, this, &DiffViewContainer::recomputeDiff);
	actionLayout->addWidget(m_recomputeButton);

	m_prevButton = new QToolButton(toolbar);
	m_prevButton->setText("◀");
	m_prevButton->setToolTip("Previous change");
	connect(m_prevButton, &QToolButton::clicked, this, &DiffViewContainer::prevChange);
	actionLayout->addWidget(m_prevButton);

	m_nextButton = new QToolButton(toolbar);
	m_nextButton->setText("▶");
	m_nextButton->setToolTip("Next change");
	connect(m_nextButton, &QToolButton::clicked, this, &DiffViewContainer::nextChange);
	actionLayout->addWidget(m_nextButton);

	m_clearButton = new QPushButton("Clear", toolbar);
	connect(m_clearButton, &QPushButton::clicked, this, &DiffViewContainer::clearDiff);
	actionLayout->addWidget(m_clearButton);

	// Indicator for the IL form currently shown in this pane.
	m_ilLabel = new QLabel(toolbar);
	m_ilLabel->setStyleSheet("font-weight: bold;");
	actionLayout->addWidget(m_ilLabel);
	actionLayout->addSpacing(8);

	// Short, always-visible similarity indicator (kept out of the truncated
	// status text so the score stays on screen regardless of function-name length).
	m_simLabel = new QLabel(toolbar);
	m_simLabel->setStyleSheet("font-weight: bold;");
	m_simLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	actionLayout->addWidget(m_simLabel);
	actionLayout->addStretch(1);

	m_legend = new QLabel(toolbar);
	m_legend->setTextFormat(Qt::RichText);
	m_legend->setToolTip("Graph diff overlay legend; the colorblind palette can be enabled in Settings");
	detailLayout->addWidget(m_legend);
	detailLayout->addSpacing(12);

	m_status = new QLabel(toolbar);
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	// Let the labels shrink below their content width so the pane splitter can
	// be dragged narrower than the toolbar text.
	m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	setStatus("Open the other binary in a split pane, select Diff View there, then press Diff.");
	detailLayout->addWidget(m_status, 1);

	// Keep the controls and the descriptive text on separate rows. Each
	// DiffViewContainer occupies only half of a side-by-side workspace, so a
	// single unbroken row otherwise gives both ViewFrames a screen-wide minimum.
	toolbarLayout->addLayout(actionLayout);
	toolbarLayout->addLayout(detailLayout);

	layout->addWidget(toolbar);

	m_disasm = new DisassemblyContainer(this, data, frame);
	layout->addWidget(m_disasm, 1);

	setILLabel(ilDisplayName(graphView()->getILViewType()));

	DiffController::instance().registerPane(this);
	setBusy(false);
	setDiffActive(false);
}

DiffViewContainer::~DiffViewContainer()
{
	DiffController::instance().unregisterPane(this);
}

void DiffViewContainer::setStatus(const QString& text)
{
	// Existing callers may publish the returned full summary again. Keep its
	// compact presentation so names and scores do not repeat beside the legend.
	m_status->setText(!m_summaryStatus.isEmpty() && text == m_summaryStatus
	    ? m_summaryDetails : text);
	m_status->setToolTip("<qt>" + text.toHtmlEscaped() + "</qt>");
}

void DiffViewContainer::setILLabel(const QString& text)
{
	m_ilLabel->setText(text);
}

void DiffViewContainer::setSimilarityLabel(const QString& text)
{
	m_simLabel->setText(text);
}

void DiffViewContainer::setDiffSummary(
    const QString& status, const QString& details, const QString& similarity)
{
	m_summaryStatus = status;
	m_summaryDetails = details;
	setSimilarityLabel(similarity);
	setStatus(status);
}

void DiffViewContainer::setBusy(bool busy)
{
	m_busy = busy;
	m_diffButton->setEnabled(!busy);
	m_recomputeButton->setEnabled(!busy);
	m_prevButton->setEnabled(!busy && m_diffActive);
	m_nextButton->setEnabled(!busy && m_diffActive);
	m_clearButton->setEnabled(!busy && m_diffActive);
}

void DiffViewContainer::setDiffActive(bool active)
{
	m_diffActive = active;
	if (!active && !m_summaryStatus.isEmpty())
	{
		m_summaryStatus.clear();
		m_summaryDetails.clear();
		setStatus("Diff cleared.");
	}
	m_prevButton->setEnabled(!m_busy && active);
	m_nextButton->setEnabled(!m_busy && active);
	m_clearButton->setEnabled(!m_busy && active);
	auto cssColor = [](const BNHighlightColor& color) {
		return QString("#%1%2%3")
		    .arg(color.r, 2, 16, QChar('0'))
		    .arg(color.g, 2, 16, QChar('0'))
		    .arg(color.b, 2, 16, QChar('0'));
	};
	const DiffPalette& colors = diffPalette();
	m_legend->setText(QString("<span style='color:%1'>■</span> removed "
	                          "<span style='color:%2'>■</span> added "
	                          "<span style='color:%3'>■</span> changed "
	                          "<span style='color:%4'>■</span> cosmetic")
	                      .arg(cssColor(colors.removed))
	                      .arg(cssColor(colors.added))
	                      .arg(cssColor(colors.changed))
	                      .arg(cssColor(colors.cosmetic)));
}

void DiffViewContainer::computeDiff()
{
	setStatus(DiffController::instance().compute(this));
}

void DiffViewContainer::recomputeDiff()
{
	setStatus(DiffController::instance().recompute(this));
}

void DiffViewContainer::clearDiff()
{
	DiffController::instance().clear();
	setStatus("Diff cleared.");
}

void DiffViewContainer::nextChange()
{
	DiffController::instance().jumpToChange(this, true);
}

void DiffViewContainer::prevChange()
{
	DiffController::instance().jumpToChange(this, false);
}

namespace {

// Forwards IL-form changes from any Diff View pane to the controller, which
// mirrors the change onto the partner pane and refreshes the indicators.
class DiffViewNotification : public UIContextNotification
{
  public:
	void OnAfterSaveFile(UIContext*, FileContext* file, ViewFrame*) override
	{
		if (file && file->getMetadata())
		{
			DiffController::instance().persistMatchesForFile(file->getMetadata().GetPtr());
			persistCorpusMatchesForFile(file->getMetadata().GetPtr());
		}
	}

	void OnViewChange(UIContext*, ViewFrame* frame, const QString& type) override
	{
		auto it = g_diffWorkspaces.find(frame);
		if (it == g_diffWorkspaces.end() || !it->second)
			return;
		QPointer<SideBySideDiffTab> workspace(it->second);
		QTimer::singleShot(0, it->second, [workspace, type]() {
			if (workspace)
				workspace->handleFrameViewChange(type);
		});
	}

	void OnILViewTypeChange(UIContext*, ViewFrame* frame, View* view,
	    const BinaryNinja::FunctionViewType& viewType) override
	{
		DiffController::instance().onILViewTypeChange(frame, view, viewType);
	}

	// Drives the synced counterpart navigation immediately when a pane moves
	// (e.g. the `g` go-to), instead of waiting for the diff controller's poll.
	void OnAddressChange(UIContext*, ViewFrame* frame, View*,
	    const ViewLocation&) override
	{
		DiffController::instance().onAddressChange(frame);
	}
};

}  // namespace

DiffViewType::DiffViewType() : ViewType("BinjaDiffView", "Binja Diff") {}

int DiffViewType::getPriority(BinaryViewRef data, const QString&)
{
	// Offer the view for any analyzed binary, but never as the default.
	return data->GetAnalysisFunctionList().empty() ? 0 : 1;
}

QWidget* DiffViewType::create(BinaryViewRef data, ViewFrame* frame)
{
	return new DiffViewContainer(frame, data);
}

void DiffViewType::init()
{
	ViewType::registerViewType(new DiffViewType());
}

extern "C"
{
	BN_DECLARE_CORE_ABI_VERSION
	BN_DECLARE_UI_ABI_VERSION

	// The plugin repository performs a headless/native scan before the UI
	// loader invokes UIPluginInit. Export a successful no-op core entry point
	// so that scan does not report this UI-only plugin as unloadable.
	BINARYNINJAPLUGIN bool CorePluginInit()
	{
		return true;
	}

	BINARYNINJAPLUGIN bool UIPluginInit()
	{
		SettingsRef settings = BinaryNinja::Settings::Instance();
		settings->RegisterGroup("binjaDiffView", "Binja Diff View");
		settings->RegisterSetting("binjaDiffView.blockMatchThreshold",
		    R"({
				"title" : "Block Match Threshold",
				"type" : "number",
				"default" : 0.5,
				"minValue" : 0.0,
				"maxValue" : 1.0,
				"description" : "Minimum IL line similarity for two basic blocks to be considered a matched pair in the diff overlay. Lower values match more aggressively; higher values leave more blocks marked added/removed."
			})");
		settings->RegisterSetting("binjaDiffView.colorblindPalette",
		    R"({
				"title" : "Colorblind-Safe Diff Palette",
				"type" : "boolean",
				"default" : false,
				"description" : "Use a colorblind-safe palette (vermillion/blue/yellow/grey) for the diff overlay instead of red/green/amber/blue. Applies to new diffs."
			})");
		settings->RegisterSetting("binjaDiffView.symbolPortMinConfidence",
		    R"({
				"title" : "Bulk Symbol Port Minimum Confidence",
				"type" : "number",
				"default" : 0.9,
				"minValue" : 0.0,
				"maxValue" : 1.0,
				"description" : "Minimum function-match confidence used by the Port all visible symbol actions. Explicitly selected rows are not confidence-filtered because the selection itself is treated as user confirmation."
			})");

		// Construct the controller now, on the GUI thread, so its sync QTimer has
		// GUI-thread affinity (a QTimer started from another thread silently fails).
		DiffController::instance();
		// Load and resolve the Rust engine here too. Analysis notifications may
		// arrive on worker threads before the user presses Diff; eager construction
		// keeps library initialization on the UI thread.
		auto& engine = bn_graph_diff::DiffEngine::instance();
		LogInfo("binja_diff_view: loaded match schema %d, extraction schema %d, "
		        "patch-signature schema %u (engine=%s)",
		    bn_graph_diff::DiffEngine::matchSchemaVersion(),
		    bn_graph_diff::DiffEngine::extractionSchemaVersion(),
		    bn_graph_diff::PatchSignature::CurrentSchemaVersion,
		    engine.available() ? "available" : "unavailable");

		DiffViewType::init();
		DiffRenderLayer::init();
		UIContext::registerNotification(new DiffViewNotification());

		UIAction::registerAction("Binja Diff: Next Change", QKeySequence("Ctrl+Alt+Shift+N"));
		UIAction::registerAction("Binja Diff: Previous Change", QKeySequence("Ctrl+Alt+Shift+P"));
		UIAction::registerAction("Binja Diff: Open Workspace");
		UIAction::registerAction("Binja Diff: Open Corpus Workspace");
		UIActionHandler::globalActions()->bindAction("Binja Diff: Next Change",
		    UIAction([](const UIActionContext&) { DiffController::instance().jumpToChange(nullptr, true); }));
		UIActionHandler::globalActions()->bindAction("Binja Diff: Previous Change",
		    UIAction([](const UIActionContext&) { DiffController::instance().jumpToChange(nullptr, false); }));
		UIActionHandler::globalActions()->bindAction("Binja Diff: Open Workspace",
		    UIAction([](const UIActionContext&) { openLoadedBinariesSideBySide(); }));
		UIActionHandler::globalActions()->bindAction("Binja Diff: Open Corpus Workspace",
		    UIAction([](const UIActionContext&) { openCorpusWorkspace(); }));
		Menu::mainMenu("Plugins")->addAction("Binja Diff View",
		    "Binja Diff: Open Workspace", "Binja Diff View");
		Menu::mainMenu("Plugins")->addAction("Binja Diff View",
		    "Binja Diff: Open Corpus Workspace", "Binja Diff View");
		return true;
	}
}
