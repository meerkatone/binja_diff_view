#include "diffresultspanel.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <vector>

#include <QtCore/QAbstractTableModel>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionHeader>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

#include "diffcontroller.h"
#include "viewframe.h"

using namespace bn_graph_diff;

namespace {

enum Column
{
	StateColumn,
	SecurityColumn,
	FunctionAColumn,
	FunctionBColumn,
	SimilarityColumn,
	ConfidenceColumn,
	AmbiguityColumn,
	MatchTypeColumn,
	InstructionDeltaColumn,
	BytesAColumn,
	BytesBColumn,
	ColumnCount
};

enum ItemRole
{
	AnchorEvidenceRole = Qt::UserRole + 100,
	RowKindRole = Qt::UserRole + 1,
	AddressARole,
	AddressBRole,
	SearchTextRole,
	MatchTypeRole,
	SimilarityRole,
	ConfidenceRole,
	AmbiguityRole,
	CfgRole,
	BasicBlockRole,
	InstructionRole,
	EdgeRole,
	NameRole,
	CallRole,
	SecurityScoreRole,
	SecurityDirectScoreRole,
	SecurityStateRole,
	SecurityEvidenceRole,
	SecurityCounterEvidenceRole,
	SortRole
};

enum RowKind
{
	MatchedRow,
	UnmatchedARow,
	UnmatchedBRow
};

struct ResultRow
{
	std::array<QString, ColumnCount> text;
	std::array<double, ColumnCount> sort {};
	std::array<bool, ColumnCount> hasSort {};
	QColor background;
	int kind = MatchedRow;
	uint64_t addressA = 0, addressB = 0;
	QString searchText, matchType, anchorEvidence;
	double similarity = -1.0, confidence = -1.0, ambiguityMargin = -1.0;
	double cfg = 0.0, basicBlock = 0.0, instruction = 0.0;
	double edge = 0.0, name = 0.0, call = 0.0;
	double securityScore = 0.0, securityDirectScore = 0.0;
	QString securityState, securityEvidence, securityCounterEvidence;
};

class ResultModel final : public QAbstractTableModel
{
  public:
	explicit ResultModel(QObject* parent) : QAbstractTableModel(parent) {}

	int rowCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
	}
	int columnCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : ColumnCount;
	}
	QVariant headerData(int section, Qt::Orientation orientation, int role) const override
	{
		static const std::array<QString, ColumnCount> headers = {"State", "Security", "Function A",
		    "Function B", "Similarity", "Confidence", "Margin", "Match", "Δ insns",
		    "Bytes A", "Bytes B"};
		if (orientation == Qt::Horizontal && role == Qt::DisplayRole
		    && section >= 0 && section < ColumnCount)
			return headers[static_cast<size_t>(section)];
		return {};
	}
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
	{
		if (!index.isValid() || index.row() < 0
		    || static_cast<size_t>(index.row()) >= m_rows.size()
		    || index.column() < 0 || index.column() >= ColumnCount)
			return {};
		const ResultRow& row = m_rows[static_cast<size_t>(index.row())];
		if (role == Qt::DisplayRole)
			return row.text[static_cast<size_t>(index.column())];
		if (role == Qt::ToolTipRole
		    && (index.column() == FunctionAColumn || index.column() == FunctionBColumn))
			return row.text[static_cast<size_t>(index.column())];
		if (role == Qt::ToolTipRole && index.column() == MatchTypeColumn
		    && row.kind != MatchedRow && !row.anchorEvidence.isEmpty())
			return row.anchorEvidence;

		if (role == Qt::BackgroundRole)
			return QBrush(row.background);
		if (role == Qt::ForegroundRole)
			return QBrush(QColor("#ffffff"));
		if (role == SortRole)
			return row.hasSort[static_cast<size_t>(index.column())]
			    ? QVariant(row.sort[static_cast<size_t>(index.column())]) : QVariant();
		switch (role)
		{
		case AnchorEvidenceRole: return row.anchorEvidence;
		case RowKindRole: return row.kind;
		case AddressARole: return QVariant::fromValue<qulonglong>(row.addressA);
		case AddressBRole: return QVariant::fromValue<qulonglong>(row.addressB);
		case SearchTextRole: return row.searchText;
		case MatchTypeRole: return row.matchType;
		case SimilarityRole: return row.similarity;
		case ConfidenceRole: return row.confidence;
		case AmbiguityRole: return row.ambiguityMargin;
		case CfgRole: return row.cfg;
		case BasicBlockRole: return row.basicBlock;
		case InstructionRole: return row.instruction;
		case EdgeRole: return row.edge;
		case NameRole: return row.name;
		case CallRole: return row.call;
		case SecurityScoreRole: return row.securityScore;
		case SecurityDirectScoreRole: return row.securityDirectScore;
		case SecurityStateRole: return row.securityState;
		case SecurityEvidenceRole: return row.securityEvidence;
		case SecurityCounterEvidenceRole: return row.securityCounterEvidence;
		default: return {};
		}
	}

	void replaceRows(std::vector<ResultRow> rows)
	{
		beginResetModel();
		m_rows = std::move(rows);
		endResetModel();
	}
	const std::vector<ResultRow>& rows() const { return m_rows; }

  private:
	std::vector<ResultRow> m_rows;
};

class ResultProxyModel final : public QSortFilterProxyModel
{
  public:
	explicit ResultProxyModel(QObject* parent) : QSortFilterProxyModel(parent)
	{
		setSortRole(SortRole);
		setDynamicSortFilter(false);
	}
	void setCriteria(QString query, QString filter, bool changedOnly, bool ambiguousOnly)
	{
		m_changedOnly = changedOnly;
		m_ambiguousOnly = ambiguousOnly;
		m_query = std::move(query);
		m_filter = std::move(filter);
		beginFilterChange();
		endFilterChange();
	}

  protected:
	bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
	{
		QVariant a = left.data(SortRole);
		QVariant b = right.data(SortRole);
		if (a.isValid() && b.isValid())
			return a.toDouble() < b.toDouble();
		return QString::localeAwareCompare(
		    left.data(Qt::DisplayRole).toString(), right.data(Qt::DisplayRole).toString()) < 0;
	}

	bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
	{
		QModelIndex index = sourceModel()->index(sourceRow, StateColumn, sourceParent);
		int kind = index.data(RowKindRole).toInt();
		double confidence = index.data(ConfidenceRole).toDouble();
		double ambiguityMargin = index.data(AmbiguityRole).toDouble();
		QString securityState = index.data(SecurityStateRole).toString();
		double securityDirectScore = index.data(SecurityDirectScoreRole).toDouble();
		QString algorithm = index.data(MatchTypeRole).toString();
		QString state = index.data(Qt::DisplayRole).toString();
		bool visible = m_query.isEmpty() || index.data(SearchTextRole).toString().contains(m_query);
		if (m_filter == "Changed matches")
			visible = visible && kind == MatchedRow && state == "Changed";
		else if (m_filter == "Byte size changed" || m_filter == "Bytes decreased (A > B)"
		    || m_filter == "Bytes increased (A < B)")
		{
			QModelIndex bytesA = sourceModel()->index(sourceRow, BytesAColumn, sourceParent);
			QModelIndex bytesB = sourceModel()->index(sourceRow, BytesBColumn, sourceParent);
			visible = visible && kind == MatchedRow
			    && (m_filter == "Bytes decreased (A > B)"
                    ? bytesA.data(SortRole).toULongLong() > bytesB.data(SortRole).toULongLong()
                    : m_filter == "Bytes increased (A < B)"
                        ? bytesA.data(SortRole).toULongLong() < bytesB.data(SortRole).toULongLong()
                        : bytesA.data(SortRole).toULongLong() != bytesB.data(SortRole).toULongLong());
		}
		else if (m_filter == "Cosmetic matches")
			visible = visible && kind == MatchedRow && state == "Cosmetic";
		else if (m_filter == "Low confidence")
			visible = visible && kind == MatchedRow && confidence < 0.85;
		else if (m_filter == "Ambiguous matches")
			visible = visible && kind == MatchedRow && ambiguityMargin < 0.01;
		else if (m_filter == "Security: strong")
			visible = visible && securityState == "Strong";
		else if (m_filter == "Security: relevant")
			visible = visible && securityAssessmentIsRelevant(
			    securityState.toStdString(), securityDirectScore, index.data(SecurityScoreRole).toDouble());
		else if (m_filter == "Matched")
			visible = visible && kind == MatchedRow;
		else if (m_filter == "Unmatched")
			visible = visible && kind != MatchedRow;
		else if (m_filter == "Unmatched A")
			visible = visible && kind == UnmatchedARow;
		else if (m_filter == "Unmatched B")
			visible = visible && kind == UnmatchedBRow;
		else if (m_filter.startsWith("Match: "))
			visible = visible && kind == MatchedRow && algorithm == m_filter.mid(7);
		return visible && (!m_changedOnly || (kind == MatchedRow && state == "Changed"))
		    && (!m_ambiguousOnly || (kind == MatchedRow && ambiguityMargin < 0.01));
	}

  private:
	QString m_query, m_filter;
	bool m_changedOnly = false, m_ambiguousOnly = false;
};

static QString functionLabel(
	const FunctionSummary& function, const std::map<uint64_t, std::string>& displayNames)
{
	auto display = displayNames.find(function.address);
	QString name = display == displayNames.end()
	    ? QString::fromStdString(function.name) : QString::fromStdString(display->second);
	if (name.isEmpty())
		name = QString("sub_%1").arg(function.address, 0, 16);
	return QString("%1  [0x%2]").arg(name).arg(function.address, 0, 16);
}

static void configureCompactCombo(QComboBox* combo, int minimumCharacters)
{
	combo->setFrame(false);
	combo->setMinimumContentsLength(minimumCharacters);
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	combo->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
}

static QString percent(double value)
{
	return QString::number(value * 100.0, 'f', 1) + "%";
}

static void setNumeric(ResultRow& row, int column, double value)
{
	row.sort[static_cast<size_t>(column)] = value;
	row.hasSort[static_cast<size_t>(column)] = true;
}

static QString skipReason(SymbolPortSkipReason reason)
{
	switch (reason)
	{
	case SymbolPortSkipReason::BelowConfidence:
		return "match confidence below the bulk threshold";
	case SymbolPortSkipReason::NoSourceSymbol:
		return "source has only a generated name";
	case SymbolPortSkipReason::TargetAlreadyNamed:
		return "target already has a meaningful name";
	case SymbolPortSkipReason::SameName:
		return "names already agree";
	case SymbolPortSkipReason::MissingFunction:
		return "function no longer exists";
	case SymbolPortSkipReason::DuplicateTarget:
		return "multiple matches target the same function";
	}
	return "unknown reason";
}

static QString planSummary(const SymbolPortPlan& plan)
{
	QStringList lines;
	lines << QString("%1 function(s) will be renamed").arg(plan.count());
	for (const auto& [reason, count] : plan.skipped)
		lines << QString("%1 skipped: %2").arg(count).arg(skipReason(reason));
	return lines.join('\n');
}

static double bulkPortConfidence()
{
	try
	{
		return BinaryNinja::Settings::Instance()->Get<double>(
		    "binjaDiffView.symbolPortMinConfidence");
	}
	catch (...)
	{
		return 0.9;
	}
}

}  // namespace

DiffResultsPanel::DiffResultsPanel(ViewFrame* left, ViewFrame* right, QWidget* parent) :
    QWidget(parent), m_left(left), m_right(right)
{
	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(6);

	QHBoxLayout* header = new QHBoxLayout();
	m_summary = new QLabel("Waiting for Rust function matching…", this);
	m_summary->setTextFormat(Qt::PlainText);
	m_summary->setStyleSheet("font-weight: bold;");
	m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
	header->addWidget(m_summary, 1);
	m_search = new QLineEdit(this);
	m_search->setPlaceholderText("Search names, addresses, or match method…");
	m_search->setClearButtonEnabled(true);
	m_search->setMinimumWidth(280);
	header->addWidget(m_search, 1);
	m_filter = new QComboBox(this);
	m_filter->addItems({"All functions", "Changed matches", "Byte size changed", "Bytes decreased (A > B)", "Bytes increased (A < B)",
	    "Security: strong", "Security: relevant", "Cosmetic matches", "Low confidence",
	    "Ambiguous matches", "Matched", "Unmatched"});
	configureCompactCombo(m_filter, 18);
	m_filter->setToolTip("Security scores are heuristics, not probabilities. Relevant includes Strong and Conflicting rows, plus Possible rows with direct operation evidence and supporting context. Byte size filters include matches regardless of security score.");
	header->addWidget(m_filter);
	layout->addLayout(header);
	QHBoxLayout* options = new QHBoxLayout();
	auto* direction = new QLabel("Baseline A → Candidate B", this);
	direction->setToolTip("Security evidence is directional. Similarity and method scores are heuristic scores, not probabilities.");
	options->addWidget(direction);
	m_changedOnly = new QCheckBox("Changed only", this);
	m_ambiguousOnly = new QCheckBox("Ambiguous only", this);
	options->addWidget(m_changedOnly);
	options->addWidget(m_ambiguousOnly);
	options->addStretch();
	auto* evidenceToggle = new QToolButton(this);
	evidenceToggle->setText("Evidence");
	evidenceToggle->setCheckable(true);
	evidenceToggle->setChecked(true);
	options->addWidget(evidenceToggle);
	layout->addLayout(options);
	connect(m_changedOnly, &QCheckBox::toggled, this, [this]() { applyFilter(); });
	connect(m_ambiguousOnly, &QCheckBox::toggled, this, [this]() { applyFilter(); });

	m_model = new ResultModel(this);
	m_proxy = new ResultProxyModel(this);
	m_proxy->setSourceModel(m_model);
	m_tree = new QTableView(this);
	m_tree->setModel(m_proxy);
	m_tree->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	m_tree->verticalHeader()->setDefaultSectionSize(m_tree->fontMetrics().height() + 6);
	m_tree->setAlternatingRowColors(true);
	m_tree->setSortingEnabled(true);
	m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	QHeaderView* tableHeader = m_tree->horizontalHeader();
	tableHeader->setSectionsMovable(true);
	tableHeader->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(tableHeader, &QHeaderView::customContextMenuRequested, this, [this, tableHeader](const QPoint& point) {
		QMenu menu(this);
		for (int column = 0; column < ColumnCount; ++column) {
			auto* action = menu.addAction(m_model->headerData(column, Qt::Horizontal).toString());
			action->setCheckable(true);
			action->setChecked(!m_tree->isColumnHidden(column));
			connect(action, &QAction::toggled, this, [this, column](bool shown) { m_tree->setColumnHidden(column, !shown); });
		}
		menu.exec(tableHeader->mapToGlobal(point));
	});
	tableHeader->setToolTip("Right-click to show or hide columns. Scores are heuristic, not calibrated probabilities.");
	tableHeader->setStretchLastSection(false);
	tableHeader->setMinimumSectionSize(48);
	for (int column = 0; column < ColumnCount; column++)
		tableHeader->setSectionResizeMode(column, QHeaderView::Interactive);
	tableHeader->resizeSection(FunctionAColumn, 300);
	tableHeader->resizeSection(FunctionBColumn, 300);
	QStyleOptionHeader headerOption;
	headerOption.initFrom(tableHeader);
	auto fitHeader = [tableHeader, &headerOption](
	                     int column, const QString& label, int minimumWidth,
	                     const QStringList& cellSamples = {}) {
		// Account for the label, both style margins, the sort arrow (any column
		// can become active), and a small separator allowance. Fixed metric
		// columns start wide enough for their contents. Interactive resize mode
		// keeps every initial width as a default while leaving every divider
		// draggable by the user.
		QStyle* style = tableHeader->style();
		int margin = style->pixelMetric(QStyle::PM_HeaderMargin, &headerOption, tableHeader);
		int sortMark = style->pixelMetric(QStyle::PM_HeaderMarkSize, &headerOption, tableHeader);
		int textWidth = tableHeader->fontMetrics().horizontalAdvance(label);
		for (const QString& sample : cellSamples)
			textWidth = std::max(textWidth,
			    tableHeader->fontMetrics().horizontalAdvance(sample));
		int width = std::max(minimumWidth,
		    textWidth + (2 * margin) + sortMark + 8);
		tableHeader->resizeSection(column, width);
	};
	fitHeader(StateColumn, "State", 85, {"Changed", "A only", "B only"});
	fitHeader(SecurityColumn, "Security", 105, {"Strong 92.0%", "Conflicting 79.0%"});
	fitHeader(SimilarityColumn, "Similarity", 90);
	fitHeader(ConfidenceColumn, "Confidence", 90);
	fitHeader(AmbiguityColumn, "Margin", 80);
	fitHeader(MatchTypeColumn, "Match", 105,
	    {"Unmatched", "SmallPrimes", "Structural", "CallGraph", "Heuristic"});
	fitHeader(InstructionDeltaColumn, "Δ insns", 75);
	fitHeader(BytesAColumn, "Bytes A", 85);
	fitHeader(BytesBColumn, "Bytes B", 85);
	layout->addWidget(m_tree, 1);

	QHBoxLayout* footer = new QHBoxLayout();
	m_details = new QLabel("Select a function pair to inspect its matching evidence.", this);
	m_details->setWordWrap(true);
	m_details->setTextFormat(Qt::PlainText);
	connect(evidenceToggle, &QToolButton::toggled, m_details, &QWidget::setVisible);
	m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
	footer->addWidget(m_details, 2);
	m_status = new QLabel(this);
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	footer->addWidget(m_status, 1);
	m_open = new QPushButton("Open in graphs", this);
	m_open->setEnabled(false);
	footer->addWidget(m_open);
	layout->addLayout(footer);

	m_filterTimer = new QTimer(this);
	m_filterTimer->setSingleShot(true);
	m_filterTimer->setInterval(100);
	connect(m_filterTimer, &QTimer::timeout, this, [this]() { applyFilter(); });
	connect(m_search, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
	connect(m_filter, &QComboBox::currentTextChanged, this, [this]() { applyFilter(); });
	connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
	    [this]() { updateSelectionDetails(); });
	connect(m_tree, &QTableView::doubleClicked, this,
	    [this](const QModelIndex&) { openSelected(); });
	connect(m_tree, &QTableView::customContextMenuRequested, this,
	    [this](const QPoint& position) { showContextMenu(position); });
	connect(m_open, &QPushButton::clicked, this, [this]() { openSelected(); });

	DiffController& controller = DiffController::instance();
	connect(&controller, &DiffController::functionMatchingStarted, this,
	    [this](ViewFrame* leftFrame, ViewFrame* rightFrame) {
		if ((leftFrame == m_left && rightFrame == m_right)
		    || (leftFrame == m_right && rightFrame == m_left))
		{
			m_summary->setText("Extracting functions and matching in Rust…");
			m_status->setText("The match phase runs in the background; existing graph navigation remains available.");
		}
	});
	connect(&controller, &DiffController::functionMatchesUpdated, this,
	    [this](ViewFrame* leftFrame, ViewFrame* rightFrame) {
		if ((leftFrame == m_left && rightFrame == m_right)
		    || (leftFrame == m_right && rightFrame == m_left))
			refresh();
	});
	connect(&controller, &DiffController::functionMatchingFailed, this,
	    [this](ViewFrame* leftFrame, ViewFrame* rightFrame, const QString& error) {
		if ((leftFrame != m_left || rightFrame != m_right)
		    && (leftFrame != m_right || rightFrame != m_left))
			return;
		m_resultSnapshot.reset();
		static_cast<ResultModel*>(m_model)->replaceRows({});
		m_open->setEnabled(false);
		m_summary->setText("Function matching failed.");
		m_details->setText("Rematch after resolving the reported error.");
		m_status->setText(error);
	});
	connect(&controller, &DiffController::matchActivationFinished, this,
	    [this](ViewFrame* leftFrame, ViewFrame* rightFrame, const QString& status) {
		if (leftFrame == m_left && rightFrame == m_right)
			m_status->setText(status);
	    });
	connect(&controller, &DiffController::symbolPortFinished, this,
	    [this](ViewFrame* leftFrame, ViewFrame* rightFrame, bool, const QString& message) {
		if (leftFrame == m_left && rightFrame == m_right)
			m_status->setText(message);
	});
}

void DiffResultsPanel::beginInitialMatch()
{
	if (DiffController::instance().matchesForFrames(m_left, m_right))
	{
		// Populate the saved match table synchronously. The per-function overlay is
		// queued for the next event turn so table rendering is never held behind
		// graph extraction, even for an unusually large current function.
		refresh();
		m_status->setText("Loaded prepared function matches.");
		QTimer::singleShot(0, this, [this]() {
			m_status->setText(DiffController::instance().computeFrame(m_left));
		});
		return;
	}
	QString status = DiffController::instance().prepareFunctionMatches(m_left);
	m_status->setText(status);
	// ViewFrame construction is normally synchronous, but defer briefly if the
	// corresponding DiffViewContainer has not registered by the first event tick.
	if (status.contains("not ready") && m_startAttempts++ < 20)
		QTimer::singleShot(50, this, [this]() { beginInitialMatch(); });
}

void DiffResultsPanel::showInitialError(const QString& error)
{
	m_resultSnapshot.reset();
	static_cast<ResultModel*>(m_model)->replaceRows({});
	m_open->setEnabled(false);
	m_summary->setText("Prepared function matches are no longer current.");
	m_details->setText("Open a new corpus workspace after analysis changes.");
	m_status->setText(error);
}

void DiffResultsPanel::setLinearMode(bool linear)
{
	m_linearMode = linear;
	m_open->setText(linear ? "Open in linear" : "Open in graphs");
}

bool DiffResultsPanel::restoreSelectedGraphDiff()
{
	QModelIndex item = m_tree->currentIndex();
	if (!item.isValid() || item.data(RowKindRole).toInt() != MatchedRow)
		return false;
	uint64_t addressA = item.data(AddressARole).toULongLong();
	uint64_t addressB = item.data(AddressBRole).toULongLong();
	m_status->setText(DiffController::instance().activateMatch(
	    m_left, m_right, addressA, addressB));
	return true;
}

void DiffResultsPanel::refresh()
{
	auto result = DiffController::instance().matchesForFrames(m_left, m_right);
	m_resultSnapshot = result;
	if (!result)
	{
		m_summary->setText("Function matches are not available yet.");
		return;
	}
	if (!result->valid)
	{
		m_summary->setText("Function matching failed.");
		m_status->setText(QString::fromStdString(result->error));
		return;
	}

	using RowIdentity = std::tuple<int, uint64_t, uint64_t>;
	auto identity = [](const QModelIndex& index) -> RowIdentity {
		return {index.data(RowKindRole).toInt(), index.data(AddressARole).toULongLong(),
		    index.data(AddressBRole).toULongLong()};
	};
	std::set<RowIdentity> selected;
	for (const auto& index : m_tree->selectionModel()->selectedRows(StateColumn))
		selected.insert(identity(index));
	const auto current = m_tree->currentIndex();
	const auto currentIdentity = current.isValid() ? std::optional<RowIdentity>(identity(current)) : std::nullopt;
	const int scroll = m_tree->verticalScrollBar()->value();
	const int horizontalScroll = m_tree->horizontalScrollBar()->value();
	const int sortColumn = m_tree->horizontalHeader()->sortIndicatorSection();
	const auto sortOrder = m_tree->horizontalHeader()->sortIndicatorOrder();
	std::vector<ResultRow> rows;
	rows.reserve(result->matches.size() + result->unmatchedA.size() + result->unmatchedB.size());
	DiffController& controller = DiffController::instance();
	const auto displayNamesA = controller.displayNamesForFrame(m_left);
	const auto displayNamesB = controller.displayNamesForFrame(m_right);
	for (const auto& match : result->matches)
	{
		ResultRow row;
		if (match.contentMatch == "Exact")
		{
			row.text[StateColumn] = "Matched";
			row.background = QColor("#1f4a1f").lighter(150);
		}
		else if (match.contentMatch == "Cosmetic")
		{
			row.text[StateColumn] = "Cosmetic";
			row.background = QColor("#1f344a").lighter(150);
		}
		else
		{
			row.text[StateColumn] = "Changed";
			row.background = QColor("#4a401f").lighter(150);
		}
		row.text[FunctionAColumn] = functionLabel(match.a, displayNamesA);
		row.text[FunctionBColumn] = functionLabel(match.b, displayNamesB);
		row.text[SimilarityColumn] = percent(match.similarity);
		row.securityState = QString::fromStdString(match.securityState);
		row.securityScore = match.securityScore;
		row.securityDirectScore = match.securityDirectScore;
		row.securityEvidence = QString::fromStdString(
		    [&match]() {
			    std::string joined;
			    for (const auto& item : match.securityEvidence)
			    {
				    if (!joined.empty()) joined += ", ";
				    joined += item;
			    }
			    return joined;
		    }());
		row.securityCounterEvidence = QString::fromStdString(
		    [&match]() {
			    std::string joined;
			    for (const auto& item : match.securityCounterEvidence)
			    {
				    if (!joined.empty()) joined += ", ";
				    joined += item;
			    }
			    return joined;
		    }());
		row.text[SecurityColumn] = row.securityState == "Unknown"
		    ? QString("—") : QString("%1 %2").arg(row.securityState, percent(match.securityScore));
		row.text[ConfidenceColumn] = percent(match.confidence);
		row.text[AmbiguityColumn] = percent(match.ambiguityMargin);
		row.text[MatchTypeColumn] = QString::fromStdString(match.matchType);
		qint64 delta = static_cast<qint64>(match.b.instructionCount)
		    - static_cast<qint64>(match.a.instructionCount);
		row.text[InstructionDeltaColumn] = QString::number(delta);
		row.text[BytesAColumn] = QString::number(static_cast<qulonglong>(match.a.size));
		row.text[BytesBColumn] = QString::number(static_cast<qulonglong>(match.b.size));
		row.kind = MatchedRow;
		row.addressA = match.a.address;
		row.addressB = match.b.address;
		row.matchType = QString::fromStdString(match.matchType);
		row.similarity = match.similarity;
		row.confidence = match.confidence;
		row.ambiguityMargin = match.ambiguityMargin;
		row.cfg = match.cfgSimilarity;
		row.basicBlock = match.basicBlockSimilarity;
		row.instruction = match.instructionSimilarity;
		row.edge = match.edgeSimilarity;
		row.name = match.nameSimilarity;
		row.call = match.callSimilarity;
		row.anchorEvidence = QString("%1 · neighbor context support %2, contradictions %3")
		    .arg(match.bindingAnchor ? "Binding unique anchor" : "Inferred pairing")
		    .arg(match.neighborSupport).arg(match.neighborContradictions);
		row.searchText = QString("%1 %2 %3 %4 %5 %6 %7")
		    .arg(row.text[FunctionAColumn], row.text[FunctionBColumn],
		        QString::fromStdString(match.a.name), QString::fromStdString(match.b.name),
		        row.text[MatchTypeColumn], row.securityState, row.securityEvidence)
		    .toLower();
		setNumeric(row, SimilarityColumn, match.similarity);
		setNumeric(row, SecurityColumn, match.securityScore);
		setNumeric(row, ConfidenceColumn, match.confidence);
		setNumeric(row, AmbiguityColumn, match.ambiguityMargin);
		setNumeric(row, InstructionDeltaColumn, static_cast<double>(delta));
		setNumeric(row, BytesAColumn, static_cast<double>(match.a.size));
		setNumeric(row, BytesBColumn, static_cast<double>(match.b.size));
		rows.push_back(std::move(row));
	}
	auto addUnmatched = [&rows, &displayNamesA, &displayNamesB](
	                        const FunctionSummary& function, bool sideA) {
		ResultRow row;
		const auto& displayNames = sideA ? displayNamesA : displayNamesB;
		row.text[StateColumn] = sideA ? "A only" : "B only";
		row.background = QColor("#4a1f1f").lighter(150);
		row.text[sideA ? FunctionAColumn : FunctionBColumn] =
		    functionLabel(function, displayNames);
		row.text[MatchTypeColumn] = function.suggestedCounterpartAddress
		    ? "Unmatched · review" : "Unmatched";
		if (function.suggestedCounterpartAddress)
			row.anchorEvidence = QString("Possible %1 counterpart: %2 at 0x%3. Body similarity %4. Shared accepted caller and callee; this is a review hint, not an accepted match.")
			    .arg(sideA ? "B" : "A")
			    .arg(QString::fromStdString(function.suggestedCounterpartName))
			    .arg(function.suggestedCounterpartAddress, 0, 16)
			    .arg(percent(function.suggestedBodySimilarity));
		row.securityState = QString::fromStdString(function.securityState);
		row.securityScore = function.securityScore;
		row.securityDirectScore = function.securityDirectScore;
		QStringList evidence;
		for (const auto& item : function.securityEvidence)
			evidence << QString::fromStdString(item);
		QStringList counterEvidence;
		for (const auto& item : function.securityCounterEvidence)
			counterEvidence << QString::fromStdString(item);
		row.securityEvidence = evidence.join(", ");
		row.securityCounterEvidence = counterEvidence.join(", ");
		row.text[SecurityColumn] = row.securityState == "Unknown"
		    ? QString("—") : QString("%1 %2").arg(row.securityState, percent(function.securityScore));
		row.text[sideA ? BytesAColumn : BytesBColumn] =
		    QString::number(static_cast<qulonglong>(function.size));
		row.kind = sideA ? UnmatchedARow : UnmatchedBRow;
		if (sideA)
			row.addressA = function.address;
		else
			row.addressB = function.address;
		row.matchType = "Unmatched";
		row.searchText = QString("%1 %2 unmatched %3 %4 %5")
		    .arg(functionLabel(function, displayNames), QString::fromStdString(function.name),
		        row.securityState, row.securityEvidence, row.anchorEvidence)
		    .toLower();
		setNumeric(row, SimilarityColumn, -1.0);
		setNumeric(row, SecurityColumn, function.securityScore);
		setNumeric(row, ConfidenceColumn, -1.0);
		setNumeric(row, AmbiguityColumn, -1.0);
		setNumeric(row, sideA ? BytesAColumn : BytesBColumn,
		    static_cast<double>(function.size));
		rows.push_back(std::move(row));
	};
	for (const auto& function : result->unmatchedA)
		addUnmatched(function, true);
	for (const auto& function : result->unmatchedB)
		addUnmatched(function, false);
	static_cast<ResultModel*>(m_model)->replaceRows(std::move(rows));
	QTimer::singleShot(0, this, [this]() { applyInitialColumnWidths(); });
	m_tree->sortByColumn(m_hasRows ? sortColumn : SecurityColumn,
	    m_hasRows ? sortOrder : Qt::DescendingOrder);
	m_hasRows = true;
	m_summary->setText(QString("%1 matches · %2 A-only · %3 B-only · "
	                           "overall %4 · matched %5 · coverage %6 · %7 s")
	                       .arg(result->matches.size())
	                       .arg(result->unmatchedA.size())
	                       .arg(result->unmatchedB.size())
	                       .arg(percent(result->similarity))
	                       .arg(percent(result->matchedSimilarity))
	                       .arg(percent(result->coverage))
	                       .arg(result->analysisTime, 0, 'f', 2));
	m_status->setText("Security direction: A is baseline, B is patched. Double-click a row to open it in the graph diff.");
	rebuildFilterChoices();
	applyFilter();
	for (int row = 0; row < m_proxy->rowCount(); ++row) {
		const auto index = m_proxy->index(row, StateColumn);
		const auto key = identity(index);
		if (selected.count(key))
			m_tree->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
		if (currentIdentity && key == *currentIdentity)
			m_tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
	}
	m_tree->verticalScrollBar()->setValue(scroll);
	m_tree->horizontalScrollBar()->setValue(horizontalScroll);
	updateSelectionDetails();
}

void DiffResultsPanel::applyInitialColumnWidths()
{
	if (m_initialColumnWidthsApplied || !m_tree || m_tree->viewport()->width() <= 0)
		return;
	QHeaderView* header = m_tree->horizontalHeader();
	int otherColumnsWidth = 0;
	for (int column = 0; column < ColumnCount; column++)
		if (column != FunctionAColumn && column != FunctionBColumn)
			otherColumnsWidth += header->sectionSize(column);
	const int availableForFunctions = m_tree->viewport()->width() - otherColumnsWidth;
	const int functionAWidth = std::max(300, availableForFunctions / 2);
	const int functionBWidth = std::max(300, availableForFunctions - functionAWidth);
	header->resizeSection(FunctionAColumn, functionAWidth);
	header->resizeSection(FunctionBColumn, functionBWidth);
	m_initialColumnWidthsApplied = true;
}

void DiffResultsPanel::rebuildFilterChoices()
{
	QString previous = m_filter->currentText();
	std::set<QString> algorithms;
	for (const ResultRow& row : static_cast<ResultModel*>(m_model)->rows())
		if (row.kind == MatchedRow)
			algorithms.insert(row.matchType);
	m_filter->blockSignals(true);
	m_filter->clear();
	m_filter->addItems({"All functions", "Changed matches", "Byte size changed", "Bytes decreased (A > B)", "Bytes increased (A < B)",
	    "Security: strong", "Security: relevant", "Cosmetic matches", "Low confidence",
	    "Ambiguous matches", "Matched", "Unmatched",
	    "Unmatched A", "Unmatched B"});
	for (const QString& algorithm : algorithms)
		m_filter->addItem("Match: " + algorithm);
	int restored = m_filter->findText(previous);
	m_filter->setCurrentIndex(restored >= 0 ? restored : 0);
	m_filter->blockSignals(false);
}

void DiffResultsPanel::applyFilter()
{
	QString query = m_search->text().trimmed().toLower();
	QString filter = m_filter->currentText();
	static_cast<ResultProxyModel*>(m_proxy)->setCriteria(std::move(query), std::move(filter),
	    m_changedOnly->isChecked(), m_ambiguousOnly->isChecked());
}

void DiffResultsPanel::updateSelectionDetails()
{
	if (auto pair = selectedUnmatchedPair())
	{
		m_open->setEnabled(true);
		m_details->setText(QString("Manual pair selected: A 0x%1 ↔ B 0x%2. Right-click either selected row to preview and port the symbol.")
		                       .arg(pair->first, 0, 16)
		                       .arg(pair->second, 0, 16));
		return;
	}
	int matchedSelected = 0;
	for (const QModelIndex& selected : m_tree->selectionModel()->selectedRows(StateColumn))
		if (selected.data(RowKindRole).toInt() == MatchedRow)
			matchedSelected++;
	if (matchedSelected > 1)
	{
		m_open->setEnabled(true);
		m_details->setText(QString("%1 matched functions selected. Right-click to preview and port their symbols in either direction.")
		                       .arg(matchedSelected));
		return;
	}
	QModelIndex item = m_tree->currentIndex();
	m_open->setEnabled(item.isValid());
	if (!item.isValid())
	{
		m_details->setText("Select a function pair to inspect its matching evidence.");
		return;
	}
	if (item.data(RowKindRole).toInt() != MatchedRow)
	{
		QString hint = item.data(AnchorEvidenceRole).toString();
		m_details->setText((hint.isEmpty() ? QString("This function has no accepted counterpart.") : hint)
		    + " Open the desired A-only and B-only functions in the graph panes, then right-click and port the current graph pair.");
		return;
	}
	QString security = item.data(SecurityStateRole).toString();
	QString securityEvidence = item.data(SecurityEvidenceRole).toString();
	QString counterEvidence = item.data(SecurityCounterEvidenceRole).toString();
	m_details->setText(item.data(AnchorEvidenceRole).toString() + "\n" + QString("Match: CFG %1 · blocks %2 · instructions %3 · edges %4 · name %5 · calls %6 · alternative margin %7\nSecurity, baseline A → candidate B: %8 %9%10%11")
	                       .arg(percent(item.data(CfgRole).toDouble()))
	                       .arg(percent(item.data(BasicBlockRole).toDouble()))
	                       .arg(percent(item.data(InstructionRole).toDouble()))
	                       .arg(percent(item.data(EdgeRole).toDouble()))
	                       .arg(percent(item.data(NameRole).toDouble()))
	                       .arg(percent(item.data(CallRole).toDouble()))
	                       .arg(percent(item.data(AmbiguityRole).toDouble()))
	                       .arg(security)
	                       .arg(percent(item.data(SecurityScoreRole).toDouble()))
	                       .arg(securityEvidence.isEmpty() ? QString() : " · " + securityEvidence)
	                       .arg(counterEvidence.isEmpty() ? QString() : " · counter: " + counterEvidence));
}

std::set<uint64_t> DiffResultsPanel::matchedAddresses(bool allVisible) const
{
	std::set<uint64_t> addresses;
	QModelIndexList candidates;
	if (allVisible)
	{
		candidates.reserve(m_proxy->rowCount());
		for (int row = 0; row < m_proxy->rowCount(); row++)
			candidates.push_back(m_proxy->index(row, StateColumn));
	}
	else
		candidates = m_tree->selectionModel()->selectedRows(StateColumn);
	for (const QModelIndex& item : candidates)
		if (item.data(RowKindRole).toInt() == MatchedRow)
			addresses.insert(item.data(AddressARole).toULongLong());
	return addresses;
}

std::optional<std::pair<uint64_t, uint64_t>> DiffResultsPanel::selectedUnmatchedPair() const
{
	std::optional<uint64_t> addressA;
	std::optional<uint64_t> addressB;
	for (const QModelIndex& item : m_tree->selectionModel()->selectedRows(StateColumn))
	{
		int kind = item.data(RowKindRole).toInt();
		if (kind == UnmatchedARow)
		{
			if (addressA)
				return std::nullopt;
			addressA = item.data(AddressARole).toULongLong();
		}
		else if (kind == UnmatchedBRow)
		{
			if (addressB)
				return std::nullopt;
			addressB = item.data(AddressBRole).toULongLong();
		}
		else
			return std::nullopt;
	}
	if (!addressA || !addressB)
		return std::nullopt;
	return std::make_pair(*addressA, *addressB);
}

void DiffResultsPanel::showContextMenu(const QPoint& position)
{
	QModelIndex clicked = m_tree->indexAt(position);
	if (clicked.isValid() && !m_tree->selectionModel()->isRowSelected(clicked.row(), {}))
	{
		m_tree->selectionModel()->clearSelection();
		m_tree->selectionModel()->select(clicked,
		    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
		m_tree->setCurrentIndex(clicked);
	}
	QMenu menu(m_tree);
	bool hasSelectedMatches = !matchedAddresses(false).empty();
	bool hasVisibleMatches = !matchedAddresses(true).empty();
	auto unmatchedPair = selectedUnmatchedPair();
	bool busy = DiffController::instance().symbolPortRunning();
	auto* pairAToB = menu.addAction("Port selected unmatched pair symbol A → B…");
	auto* pairBToA = menu.addAction("Port selected unmatched pair symbol B → A…");
	pairAToB->setEnabled(unmatchedPair.has_value() && !busy);
	pairBToA->setEnabled(unmatchedPair.has_value() && !busy);
	connect(pairAToB, &QAction::triggered, this,
	    [this, unmatchedPair]() { portSymbols(SymbolPortDirection::AToB, false, false, unmatchedPair); });
	connect(pairBToA, &QAction::triggered, this,
	    [this, unmatchedPair]() { portSymbols(SymbolPortDirection::BToA, false, false, unmatchedPair); });
	menu.addSeparator();
	auto* currentAToB = menu.addAction("Port current graph symbol A → B…");
	auto* currentBToA = menu.addAction("Port current graph symbol B → A…");
	currentAToB->setEnabled(!busy);
	currentBToA->setEnabled(!busy);
	connect(currentAToB, &QAction::triggered, this,
	    [this]() { portSymbols(SymbolPortDirection::AToB, false, true); });
	connect(currentBToA, &QAction::triggered, this,
	    [this]() { portSymbols(SymbolPortDirection::BToA, false, true); });
	menu.addSeparator();
	auto* selectedAToB = menu.addAction("Port selected symbols A → B…");
	auto* selectedBToA = menu.addAction("Port selected symbols B → A…");
	selectedAToB->setEnabled(hasSelectedMatches && !busy);
	selectedBToA->setEnabled(hasSelectedMatches && !busy);
	connect(selectedAToB, &QAction::triggered, this,
	    [this]() { portSymbols(SymbolPortDirection::AToB, false); });
	connect(selectedBToA, &QAction::triggered, this,
	    [this]() { portSymbols(SymbolPortDirection::BToA, false); });
	menu.addSeparator();
	auto* visibleAToB = menu.addAction("Port all visible high-confidence symbols A → B…");
	auto* visibleBToA = menu.addAction("Port all visible high-confidence symbols B → A…");
	visibleAToB->setEnabled(hasVisibleMatches && !busy);
	visibleBToA->setEnabled(hasVisibleMatches && !busy);
	connect(visibleAToB, &QAction::triggered, this,
	    [this]() { portSymbols(SymbolPortDirection::AToB, true); });
	connect(visibleBToA, &QAction::triggered, this,
	    [this]() { portSymbols(SymbolPortDirection::BToA, true); });
	menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void DiffResultsPanel::portSymbols(
	SymbolPortDirection direction, bool allVisible, bool currentPair,
	std::optional<std::pair<uint64_t, uint64_t>> addressPair)
{
	std::set<uint64_t> addresses = matchedAddresses(allVisible);
	if (addresses.empty() && !currentPair && !addressPair)
		return;
	SymbolPortOptions options;
	options.direction = direction;
	// A hand-picked match is an explicit decision. Bulk operations retain a
	// configurable guard because a wrong name is more damaging than no name.
	options.minConfidence = allVisible ? bulkPortConfidence() : 0.0;
	auto makePlan = [this, currentPair, addressPair, &addresses](const SymbolPortOptions& requested) {
		if (addressPair)
			return DiffController::instance().planAddressPairSymbolPort(
			    m_left, m_right, addressPair->first, addressPair->second, requested);
		if (currentPair)
			return DiffController::instance().planCurrentSymbolPort(
			    m_left, m_right, requested);
		return DiffController::instance().planSymbolPort(
		    m_left, m_right, addresses, requested);
	};
	SymbolPortPlan plan = makePlan(options);
	size_t conflicts = plan.skippedCount(SymbolPortSkipReason::TargetAlreadyNamed);
	if (plan.count() == 0 && conflicts == 0)
	{
		QMessageBox::information(this, "Port symbols", "Nothing to port.\n\n" + planSummary(plan));
		return;
	}

	QString directionText = direction == SymbolPortDirection::AToB ? "A → B" : "B → A";
	QStringList preview;
	for (size_t index = 0; index < std::min<size_t>(plan.renames.size(), 10); index++)
	{
		const auto& rename = plan.renames[index];
		preview << QString("0x%1 (%2) → 0x%3 (%4 → %5)  [%6% confidence]")
		               .arg(rename.sourceAddress, 0, 16)
		               .arg(QString::fromStdString(rename.newShortName))
		               .arg(rename.targetAddress, 0, 16)
		               .arg(QString::fromStdString(rename.oldName))
		               .arg(QString::fromStdString(rename.newShortName))
		               .arg(rename.confidence * 100.0, 0, 'f', 1);
	}
	if (plan.renames.size() > 10)
		preview << QString("…and %1 more").arg(plan.renames.size() - 10);
	QStringList overwritePreview;
	if (conflicts)
	{
		SymbolPortOptions overwriteOptions = options;
		overwriteOptions.overwrite = true;
		SymbolPortPlan overwritePlan = makePlan(overwriteOptions);
		for (const auto& rename : overwritePlan.renames)
		{
			if (isGeneratedFunctionName(rename.oldName))
				continue;
			overwritePreview << QString("0x%1: %2 → %3")
			                        .arg(rename.targetAddress, 0, 16)
			                        .arg(QString::fromStdString(rename.oldName))
			                        .arg(QString::fromStdString(rename.newShortName));
			if (overwritePreview.size() == 6)
				break;
		}
		if (overwritePreview.size() == 6)
			overwritePreview.back() = "…additional named targets omitted";
	}

	QMessageBox confirmation(this);
	confirmation.setWindowTitle("Port symbols");
	confirmation.setIcon(QMessageBox::Question);
	confirmation.setTextFormat(Qt::PlainText);
	confirmation.setText(QString("Port function symbols %1?").arg(directionText));
	confirmation.setInformativeText(planSummary(plan)
	    + (preview.empty() ? QString() : QString("\n\nPreview:\n") + preview.join('\n'))
	    + (overwritePreview.empty() ? QString()
	                                : QString("\n\nWould overwrite:\n")
	                                      + overwritePreview.join('\n'))
	    + "\n\nThe batch will be one Binary Ninja undo step.");
	QAbstractButton* safeButton = nullptr;
	if (plan.count())
		safeButton = confirmation.addButton(
		    QString("Port %1 safe name(s)").arg(plan.count()), QMessageBox::AcceptRole);
	QAbstractButton* overwriteButton = nullptr;
	if (conflicts)
		overwriteButton = confirmation.addButton(
		    QString("Include %1 named target(s)").arg(conflicts), QMessageBox::DestructiveRole);
	confirmation.addButton(QMessageBox::Cancel);
	confirmation.exec();
	if (confirmation.clickedButton() == overwriteButton)
	{
		options.overwrite = true;
		plan = makePlan(options);
	}
	else if (confirmation.clickedButton() != safeButton)
		return;

	if (!plan.count())
	{
		QMessageBox::information(this, "Port symbols", "Nothing to port.\n\n" + planSummary(plan));
		return;
	}
	if (DiffController::instance().applySymbolPort(m_left, m_right, plan, options))
		m_status->setText(QString("Porting %1 symbol(s) %2 in the background…")
		                      .arg(plan.count()).arg(directionText));
	else
		m_status->setText("Could not start the symbol port; another operation may already be running.");
}

void DiffResultsPanel::openSelected()
{
	QModelIndex item = m_tree->currentIndex();
	if (!item.isValid())
		return;
	int kind = item.data(RowKindRole).toInt();
	QString status;
	if (m_linearMode)
	{
		auto navigateLinear = [](ViewFrame* frame, uint64_t address) {
			if (!frame)
				return false;
			QString type = QString("Linear:%1").arg(frame->getCurrentDataType());
			return frame->navigate(type, address);
		};
		bool navigated = false;
		if (kind == MatchedRow)
		{
			uint64_t addressA = item.data(AddressARole).toULongLong();
			uint64_t addressB = item.data(AddressBRole).toULongLong();
			bool navigatedA = navigateLinear(m_left, addressA);
			bool navigatedB = navigateLinear(m_right, addressB);
			navigated = navigatedA && navigatedB;
			status = navigated
			    ? DiffController::instance().activateLinearMatch(
			          m_left, m_right, addressA, addressB)
			    : "Could not navigate one of the linear panes.";
		}
		else if (kind == UnmatchedARow)
		{
			DiffController::instance().clear();
			uint64_t address = item.data(AddressARole).toULongLong();
			navigated = navigateLinear(m_left, address);
			status = navigated ? QString("Showing unmatched function at 0x%1 in linear view.").arg(address, 0, 16)
			                   : "Could not navigate the left linear pane.";
		}
		else
		{
			DiffController::instance().clear();
			uint64_t address = item.data(AddressBRole).toULongLong();
			navigated = navigateLinear(m_right, address);
			status = navigated ? QString("Showing unmatched function at 0x%1 in linear view.").arg(address, 0, 16)
			                   : "Could not navigate the right linear pane.";
		}
		m_status->setText(status);
		return;
	}
	if (kind == MatchedRow)
	{
		uint64_t addressA = item.data(AddressARole).toULongLong();
		uint64_t addressB = item.data(AddressBRole).toULongLong();
		status = DiffController::instance().activateMatch(m_left, m_right, addressA, addressB);
	}
	else if (kind == UnmatchedARow)
	{
		status = DiffController::instance().navigateUnmatched(
		    m_left, item.data(AddressARole).toULongLong());
	}
	else
	{
		status = DiffController::instance().navigateUnmatched(
		    m_right, item.data(AddressBRole).toULongLong());
	}
	m_status->setText(status);
}
