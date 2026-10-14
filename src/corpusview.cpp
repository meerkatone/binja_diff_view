#include "corpusview.h"

#include <QtCore/QAbstractTableModel>
#include <QtCore/QFileInfo>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFutureWatcher>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QLinearGradient>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "corpusengine.h"
#include "corpuspersistence.h"
#include "diffcontroller.h"
#include "diffview.h"
#include "filecontext.h"
#include "patchsignature.h"
#include "uicontext.h"

using namespace BinaryNinja;
using namespace bn_graph_diff;

namespace {

class CorpusWorkspace;
std::set<CorpusWorkspace*> g_corpusWorkspaces;

struct FamilyBuildResult
{
	std::vector<FunctionFamily> families;
	qint64 elapsedMilliseconds = 0;
	std::string error;
};

QString paneLabel(const DiffWorkspacePane& pane)
{
	QString filename = pane.fileContext ? pane.fileContext->getFilename() : QString();
	QString name = filename.isEmpty() ? QString("unknown") : QFileInfo(filename).fileName();
	return QString("%1  [%2]").arg(name, pane.dataType.isEmpty() ? QString("default") : pane.dataType);
}

void configureCompactCombo(QComboBox* combo, int minimumCharacters)
{
	combo->setMinimumContentsLength(minimumCharacters);
	combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	combo->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
}

void configureContentSizedColumns(QTableView* table)
{
	QHeaderView* header = table->horizontalHeader();
	header->setStretchLastSection(false);
	// Corpus models change while comparisons finish and when filters are applied.
	// ResizeToContents keeps both header and cell text visible after those changes;
	// inspecting every row avoids missing a long function name outside Qt's
	// default sample window.
	header->setResizeContentsPrecision(-1);
	header->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
}

class CorpusSetupDialog : public QDialog
{
  public:
	CorpusSetupDialog(const std::vector<DiffWorkspacePane>& panes, QWidget* parent = nullptr) :
	    QDialog(parent), m_panes(panes)
	{
		setWindowTitle("Binja Diff corpus setup");
		setMinimumWidth(560);
		auto* layout = new QVBoxLayout(this);
		auto* description = new QLabel(
		    "Select at least three analyzed binaries, choose a comparison mode, and review the planned pair count.", this);
		description->setWordWrap(true);
		layout->addWidget(description);

		m_files = new QListWidget(this);
		m_files->setDragDropMode(QAbstractItemView::InternalMove);
		m_files->setDefaultDropAction(Qt::MoveAction);
		for (size_t index = 0; index < panes.size(); index++)
		{
			auto* item = new QListWidgetItem(paneLabel(panes[index]), m_files);
			item->setData(Qt::UserRole, static_cast<int>(index));
			QString filename = panes[index].fileContext
			    ? QFileInfo(panes[index].fileContext->getFilename()).fileName() : QString();
			item->setData(Qt::UserRole + 1,
			    filename.isEmpty() ? item->text() : filename);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			bool usable = panes[index].data && panes[index].data->GetTypeName() != "Raw"
			    && panes[index].data->GetAnalysisState() == IdleState;
			item->setCheckState(usable ? Qt::Checked : Qt::Unchecked);
			if (!usable)
			{
				item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
				item->setToolTip("This tab does not expose an idle analyzed BinaryView");
			}
		}
		sortFilesByRelease();
		layout->addWidget(m_files, 1);
		auto* ordering = new QHBoxLayout();
		auto* autoSort = new QPushButton("Auto-sort oldest to newest", this);
		autoSort->setToolTip(
		    "Naturally sort filename versions. Drag rows afterward to override the result.");
		ordering->addWidget(autoSort);
		auto* orderingHint = new QLabel(
		    "Filename numbers are compared numerically; prereleases sort before final releases.", this);
		orderingHint->setWordWrap(true);
		ordering->addWidget(orderingHint, 1);
		layout->addLayout(ordering);

		auto* form = new QFormLayout();
		m_mode = new QComboBox(this);
		m_mode->addItems({"Reference vs many", "Ordered lineage", "All pairs"});
		form->addRow("Mode", m_mode);
		m_reference = new QComboBox(this);
		form->addRow("Reference", m_reference);
		m_referenceOverlay = new QCheckBox("Compare the first build with every later build", this);
		form->addRow("Lineage overlay", m_referenceOverlay);
		m_pairCount = new QLabel(this);
		m_pairCount->setTextFormat(Qt::PlainText);
		form->addRow("Planned comparisons", m_pairCount);
		m_reuseCache = new QCheckBox("Reuse cached pair results", this);
		m_reuseCache->setChecked(true);
		form->addRow(QString(), m_reuseCache);
		layout->addLayout(form);

		m_buttons = new QDialogButtonBox(
		    QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		m_buttons->button(QDialogButtonBox::Ok)->setText("Start");
		connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(m_files, &QListWidget::itemChanged, this,
		    [this](QListWidgetItem*) { refresh(); });
		connect(m_files->model(), &QAbstractItemModel::rowsMoved, this,
		    [this]() { refresh(); });
		connect(m_mode, &QComboBox::currentIndexChanged, this,
		    [this](int) { refresh(); });
		connect(m_referenceOverlay, &QCheckBox::toggled, this,
		    [this](bool) { refresh(); });
		connect(autoSort, &QPushButton::clicked, this, [this]() {
			sortFilesByRelease();
			refresh();
		});
		layout->addWidget(m_buttons);
		refresh();
	}

	std::vector<size_t> selectedIndices() const
	{
		std::vector<size_t> result;
		for (int row = 0; row < m_files->count(); row++)
		{
			QListWidgetItem* item = m_files->item(row);
			if (item->checkState() == Qt::Checked)
				result.push_back(static_cast<size_t>(item->data(Qt::UserRole).toInt()));
		}
		return result;
	}

	std::optional<size_t> referenceIndex() const
	{
		if (m_reference->currentIndex() < 0)
			return std::nullopt;
		return static_cast<size_t>(m_reference->currentData().toInt());
	}

	bool reuseCachedPairs() const { return m_reuseCache->isChecked(); }
	CorpusMode mode() const
	{
		return m_mode->currentIndex() == 1 ? CorpusMode::Lineage
		    : m_mode->currentIndex() == 2 ? CorpusMode::AllPairs
		                                  : CorpusMode::Reference;
	}
	bool referenceOverlay() const { return m_referenceOverlay->isChecked(); }

  private:
	void sortFilesByRelease()
	{
		std::vector<QListWidgetItem*> items;
		items.reserve(static_cast<size_t>(m_files->count()));
		while (m_files->count())
			items.push_back(m_files->takeItem(0));
		std::stable_sort(items.begin(), items.end(), [](const QListWidgetItem* left,
		    const QListWidgetItem* right) {
			return naturalReleaseNameLess(
			    left->data(Qt::UserRole + 1).toString().toStdString(),
			    right->data(Qt::UserRole + 1).toString().toStdString());
		});
		for (auto* item : items)
			m_files->addItem(item);
	}

	bool hasDuplicateFiles() const
	{
		std::set<QString> paths;
		for (size_t index : selectedIndices())
		{
			QString filename = m_panes[index].fileContext
			    ? m_panes[index].fileContext->getFilename() : QString();
			if (filename.isEmpty())
				continue;
			QFileInfo info(filename);
			QString identity = info.canonicalFilePath();
			if (identity.isEmpty())
				identity = info.absoluteFilePath();
			if (!identity.isEmpty() && !paths.insert(identity).second)
				return true;
		}
		return false;
	}

	void refresh()
	{
		int previous = m_reference->currentData().toInt();
		m_reference->blockSignals(true);
		m_reference->clear();
		for (size_t index : selectedIndices())
			m_reference->addItem(paneLabel(m_panes[index]), static_cast<int>(index));
		int restore = m_reference->findData(previous);
		m_reference->setCurrentIndex(restore >= 0 ? restore : 0);
		m_reference->blockSignals(false);
		size_t count = selectedIndices().size();
		bool duplicates = hasDuplicateFiles();
		m_reference->setEnabled(mode() == CorpusMode::Reference);
		m_referenceOverlay->setVisible(mode() == CorpusMode::Lineage);
		std::vector<CorpusBinaryId> order;
		for (size_t index = 0; index < count; index++)
			order.push_back(static_cast<CorpusBinaryId>(index));
		std::optional<CorpusBinaryId> reference;
		if (mode() == CorpusMode::Reference && count)
			reference = 0;
		CorpusPlan preview = count >= 2 ? buildCorpusPlan(mode(), order, reference,
		    referenceOverlay()) : CorpusPlan {};
		if (duplicates)
			m_pairCount->setText("Duplicate files selected");
		else if (count >= 2 && !preview.valid)
			m_pairCount->setText(QString::fromStdString(preview.error));
		else
			m_pairCount->setText(QString::number(preview.edges.size()));
		m_buttons->button(QDialogButtonBox::Ok)->setEnabled(
		    count >= 3 && !duplicates && preview.valid);
	}

	const std::vector<DiffWorkspacePane>& m_panes;
	QListWidget* m_files = nullptr;
	QComboBox* m_mode = nullptr;
	QComboBox* m_reference = nullptr;
	QCheckBox* m_referenceOverlay = nullptr;
	QLabel* m_pairCount = nullptr;
	QCheckBox* m_reuseCache = nullptr;
	QDialogButtonBox* m_buttons = nullptr;
};

enum class MatrixMetric
{
	SourceFunctionCoverage,
	SourceInstructionCoverage,
	MeanSimilarity,
	MeanConfidence,
	AcceptedMatches,
	ChangedFamilies
};

class CorpusMatrixModel : public QAbstractTableModel
{
  public:
	CorpusMatrixModel(std::vector<CorpusBinary> binaries, CorpusPlan plan,
	    QObject* parent = nullptr) :
	    QAbstractTableModel(parent), m_binaries(std::move(binaries)), m_plan(std::move(plan))
	{
		for (const auto& edge : m_plan.edges)
			m_states[edge] = CorpusPairState::Queued;
	}

	int rowCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_binaries.size());
	}
	int columnCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_binaries.size());
	}

	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override
	{
		if (role != Qt::DisplayRole || section < 0
		    || static_cast<size_t>(section) >= m_binaries.size())
			return {};
		Q_UNUSED(orientation);
		return QString::fromStdString(m_binaries[static_cast<size_t>(section)].displayName);
	}

	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
	{
		if (!index.isValid() || index.row() < 0 || index.column() < 0)
			return {};
		if (index.row() == index.column())
			return role == Qt::DisplayRole ? QVariant(QString::fromUtf8("—")) : QVariant();
		bool inverted = false;
		CorpusPairResultRef result = pairAt(index.row(), index.column(), &inverted);
		CorpusEdgeKey requested {m_binaries[static_cast<size_t>(index.row())].id,
		    m_binaries[static_cast<size_t>(index.column())].id};
		CorpusPairState state = stateFor(requested);
		if (role == Qt::TextAlignmentRole)
			return Qt::AlignCenter;
		if (role == Qt::DisplayRole)
		{
			if (!result)
			{
				switch (state)
				{
				case CorpusPairState::Queued: return "Queued";
				case CorpusPairState::Running: return "Running";
				case CorpusPairState::Cancelled: return "Cancelled";
				default: return {};
				}
			}
			if (result->state == CorpusPairState::Failed)
				return "Failed";
			if (result->state == CorpusPairState::Cancelled)
				return "Cancelled";
			CorpusPairMetrics metrics = inverted ? invertCorpusMetrics(result->metrics) : result->metrics;
			switch (m_metric)
			{
			case MatrixMetric::SourceFunctionCoverage:
				return QString::number(metrics.sourceCoverage * 100.0, 'f', 1) + "%";
			case MatrixMetric::SourceInstructionCoverage:
				return QString::number(metrics.instructionCoverageSource * 100.0, 'f', 1) + "%";
			case MatrixMetric::MeanSimilarity:
				return QString::number(metrics.meanSimilarity * 100.0, 'f', 1) + "%";
			case MatrixMetric::MeanConfidence:
				return QString::number(metrics.meanConfidence * 100.0, 'f', 1) + "%";
			case MatrixMetric::AcceptedMatches:
				return QString::number(metrics.acceptedMatchCount);
			case MatrixMetric::ChangedFamilies:
				return QString::number(changedFamilies(result->edge));
			}
		}
		if (role == Qt::ToolTipRole && result)
		{
			CorpusPairMetrics metrics = inverted ? invertCorpusMetrics(result->metrics) : result->metrics;
			CorpusPairMetrics raw = inverted ? invertCorpusMetrics(result->rawMetrics) : result->rawMetrics;
			if (!result->error.empty())
				return QString::fromStdString(result->error);
			return QString("Row to column\nFiltered functions: %1/%2 (%3%)\nFiltered instructions: %4/%5 (%6%)\nRaw coverage: %7% functions, %8% instructions\nMean similarity: %9%\nConfidence: %10%\n%11")
			    .arg(metrics.acceptedMatchCount)
			    .arg(metrics.sourceFunctionCount)
			    .arg(metrics.sourceCoverage * 100.0, 0, 'f', 1)
			    .arg(metrics.sourceInstructionCount - metrics.sourceOnlyInstructionCount)
			    .arg(metrics.sourceInstructionCount)
			    .arg(metrics.instructionCoverageSource * 100.0, 0, 'f', 1)
			    .arg(raw.sourceCoverage * 100.0, 0, 'f', 1)
			    .arg(raw.instructionCoverageSource * 100.0, 0, 'f', 1)
			    .arg(metrics.meanSimilarity * 100.0, 0, 'f', 1)
			    .arg(metrics.meanConfidence * 100.0, 0, 'f', 1)
			    .arg(result->state == CorpusPairState::Cached ? "Restored from pair cache" : "Matched in this run");
		}
		if (role == Qt::BackgroundRole)
		{
			if (result && result->state == CorpusPairState::Failed)
				return QColor(90, 35, 35);
			if (result && result->state == CorpusPairState::Cached)
				return QColor(35, 55, 80);
			if (result && result->matches)
				return QColor(35, 75, 45);
			if (state == CorpusPairState::Running)
				return QColor(85, 70, 25);
		}
		return {};
	}

	void setMetric(MatrixMetric metric)
	{
		m_metric = metric;
		if (!m_binaries.empty())
			emit dataChanged(index(0, 0),
			    index(static_cast<int>(m_binaries.size() - 1),
			        static_cast<int>(m_binaries.size() - 1)));
	}

	void setFamilies(const std::vector<FunctionFamily>& families)
	{
		m_changedFamilies.clear();
		std::map<CorpusEdgeKey, std::set<FunctionFamilyId>> changed;
		for (const auto& family : families)
			for (const auto& link : family.acceptedLinks)
				if (link.similarity < 1.0)
					changed[link.origin].insert(family.id);
		for (const auto& [edge, ids] : changed)
			m_changedFamilies[edge] = ids.size();
		if (!m_binaries.empty())
			emit dataChanged(index(0, 0),
			    index(static_cast<int>(m_binaries.size() - 1),
			        static_cast<int>(m_binaries.size() - 1)));
	}

	void setRunning(CorpusEdgeKey edge)
	{
		m_states[edge] = CorpusPairState::Running;
		refreshEdge(edge);
	}

	void setResult(CorpusPairResultRef result)
	{
		if (!result)
			return;
		m_results[result->edge] = result;
		m_states[result->edge] = result->state;
		refreshEdge(result->edge);
	}

	void invalidateBinary(CorpusBinaryId binary)
	{
		for (auto it = m_results.begin(); it != m_results.end();)
			if (it->first.source == binary || it->first.target == binary)
				it = m_results.erase(it);
			else
				++it;
		for (auto& [edge, state] : m_states)
			if (edge.source == binary || edge.target == binary)
				state = CorpusPairState::Invalidated;
		for (auto it = m_changedFamilies.begin(); it != m_changedFamilies.end();)
			if (it->first.source == binary || it->first.target == binary)
				it = m_changedFamilies.erase(it);
			else
				++it;
		if (!m_binaries.empty())
			emit dataChanged(index(0, 0),
			    index(static_cast<int>(m_binaries.size() - 1),
			        static_cast<int>(m_binaries.size() - 1)));
	}

	CorpusPairResultRef pairAt(int row, int column, bool* inverted = nullptr) const
	{
		if (row < 0 || column < 0 || static_cast<size_t>(row) >= m_binaries.size()
		    || static_cast<size_t>(column) >= m_binaries.size())
			return {};
		CorpusEdgeKey direct {m_binaries[static_cast<size_t>(row)].id,
		    m_binaries[static_cast<size_t>(column)].id};
		auto found = m_results.find(direct);
		if (found != m_results.end())
		{
			if (inverted)
				*inverted = false;
			return found->second;
		}
		CorpusEdgeKey reverse {direct.target, direct.source};
		found = m_results.find(reverse);
		if (found != m_results.end())
		{
			if (inverted)
				*inverted = true;
			return found->second;
		}
		return {};
	}

  private:
	size_t changedFamilies(CorpusEdgeKey edge) const
	{
		auto direct = m_changedFamilies.find(edge);
		if (direct != m_changedFamilies.end())
			return direct->second;
		auto reverse = m_changedFamilies.find({edge.target, edge.source});
		return reverse == m_changedFamilies.end() ? 0 : reverse->second;
	}

	CorpusPairState stateFor(CorpusEdgeKey edge) const
	{
		auto direct = m_states.find(edge);
		if (direct != m_states.end())
			return direct->second;
		auto reverse = m_states.find({edge.target, edge.source});
		return reverse == m_states.end() ? CorpusPairState::Invalidated : reverse->second;
	}

	void refreshEdge(CorpusEdgeKey edge)
	{
		auto row = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [edge](const CorpusBinary& binary) { return binary.id == edge.source; });
		auto column = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [edge](const CorpusBinary& binary) { return binary.id == edge.target; });
		if (row == m_binaries.end() || column == m_binaries.end())
			return;
		int r = static_cast<int>(std::distance(m_binaries.begin(), row));
		int c = static_cast<int>(std::distance(m_binaries.begin(), column));
		emit dataChanged(index(r, c), index(r, c));
		emit dataChanged(index(c, r), index(c, r));
	}

	std::vector<CorpusBinary> m_binaries;
	CorpusPlan m_plan;
	MatrixMetric m_metric = MatrixMetric::SourceInstructionCoverage;
	std::map<CorpusEdgeKey, CorpusPairState> m_states;
	std::map<CorpusEdgeKey, CorpusPairResultRef> m_results;
	std::map<CorpusEdgeKey, size_t> m_changedFamilies;
};

class CorpusFamilyModel : public QAbstractTableModel
{
  public:
	CorpusFamilyModel(std::vector<CorpusBinary> binaries, QObject* parent = nullptr) :
	    QAbstractTableModel(parent), m_binaries(std::move(binaries))
	{}

	int rowCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_families.size());
	}
	int columnCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_binaries.size() + 7);
	}

	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override
	{
		if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0)
			return {};
		if (section == 0) return "Family";
		if (section == 1) return "State";
		if (section >= 2 && static_cast<size_t>(section - 2) < m_binaries.size())
			return QString::fromStdString(m_binaries[static_cast<size_t>(section - 2)].displayName);
		int metric = section - static_cast<int>(m_binaries.size()) - 2;
		switch (metric)
		{
		case 0: return "Min score";
		case 1: return "Confidence";
		case 2: return "Evidence";
		case 3: return "First seen";
		case 4: return "Last seen";
		default: return {};
		}
	}

	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
	{
		if (!index.isValid() || index.row() < 0
		    || static_cast<size_t>(index.row()) >= m_families.size())
			return {};
		const FunctionFamily& family = m_families[static_cast<size_t>(index.row())];
		if (role == Qt::UserRole)
			return static_cast<int>(family.state);
		if (role == Qt::UserRole + 1)
			return static_cast<qulonglong>(family.id);
		if (role == Qt::TextAlignmentRole && (index.column() == 0
		    || index.column() >= static_cast<int>(m_binaries.size()) + 2))
			return Qt::AlignCenter;
		if (role == Qt::BackgroundRole)
		{
			switch (family.state)
			{
			case FunctionFamilyState::Identical: return QColor(30, 70, 40);
			case FunctionFamilyState::Changed: return QColor(80, 68, 28);
			case FunctionFamilyState::Missing:
			case FunctionFamilyState::Unique: return QColor(78, 38, 38);
			case FunctionFamilyState::Ambiguous: return QColor(65, 50, 85);
			case FunctionFamilyState::Conflicting: return QColor(100, 38, 55);
			}
		}
		if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
			return {};
		if (index.column() == 0)
			return QString::number(family.id);
		if (index.column() == 1)
			return functionFamilyStateName(family.state);
		if (index.column() >= 2
		    && static_cast<size_t>(index.column() - 2) < m_binaries.size())
		{
			CorpusBinaryId binary = m_binaries[static_cast<size_t>(index.column() - 2)].id;
			auto member = family.members.find(binary);
			if (member == family.members.end())
				return {};
			QString text = QString("%1 @ 0x%2")
			    .arg(QString::fromStdString(member->second.name))
			    .arg(member->second.address, 0, 16);
			auto classification = m_patchClassifications.find({family.id, binary});
			if (classification != m_patchClassifications.end())
			{
				if (role == Qt::ToolTipRole)
				{
					QString detail = patchStateName(classification->second.state);
					for (const auto& evidence : classification->second.evidence)
						detail += "\n" + QString::fromStdString(evidence);
					return detail;
				}
				text += QString(" [%1]").arg(patchStateName(classification->second.state));
			}
			return text;
		}
		int metric = index.column() - static_cast<int>(m_binaries.size()) - 2;
		if (metric == 0)
			return family.acceptedLinks.empty() ? QVariant()
			    : QVariant(QString::number(family.minimumSimilarity * 100.0, 'f', 1) + "%");
		if (metric == 1)
			return family.acceptedLinks.empty() ? QVariant()
			    : QVariant(QString::number(family.minimumConfidence * 100.0, 'f', 1) + "%");
		if (metric == 2)
			return QString("%1 accepted, %2 rejected")
			    .arg(family.acceptedLinks.size()).arg(family.rejectedLinks.size());
		if (metric == 3 || metric == 4)
		{
			auto findPresent = [this, &family](bool reverse) -> QString {
				if (!reverse)
				{
					for (const auto& binary : m_binaries)
						if (family.members.count(binary.id))
							return QString::fromStdString(binary.displayName);
				}
				else
				{
					for (auto binary = m_binaries.rbegin(); binary != m_binaries.rend(); ++binary)
						if (family.members.count(binary->id))
							return QString::fromStdString(binary->displayName);
				}
				return {};
			};
			return findPresent(metric == 4);
		}
		return {};
	}

	void setFamilies(std::vector<FunctionFamily> families)
	{
		beginResetModel();
		m_families = std::move(families);
		endResetModel();
	}

	void setPatchClassifications(FunctionFamilyId family,
		const std::map<CorpusBinaryId, PatchClassification>& classifications)
	{
		for (auto it = m_patchClassifications.begin(); it != m_patchClassifications.end();)
			if (it->first.first == family)
				it = m_patchClassifications.erase(it);
			else
				++it;
		for (const auto& [binary, classification] : classifications)
			m_patchClassifications[{family, binary}] = classification;
		for (size_t row = 0; row < m_families.size(); row++)
			if (m_families[row].id == family)
			{
				emit dataChanged(index(static_cast<int>(row), 2),
				    index(static_cast<int>(row), static_cast<int>(m_binaries.size() + 1)));
				break;
			}
	}

	const FunctionFamily* familyAt(int row) const
	{
		return row < 0 || static_cast<size_t>(row) >= m_families.size()
		    ? nullptr : &m_families[static_cast<size_t>(row)];
	}

	std::optional<FunctionOccurrence> occurrenceAt(int row, int column) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family || column < 2 || static_cast<size_t>(column - 2) >= m_binaries.size())
			return std::nullopt;
		CorpusBinaryId binary = m_binaries[static_cast<size_t>(column - 2)].id;
		auto occurrence = family->members.find(binary);
		return occurrence == family->members.end() ? std::nullopt
		                                         : std::optional<FunctionOccurrence>(occurrence->second);
	}

	QString searchText(int row) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family)
			return {};
		QString text = QString("%1 %2 ").arg(family->id)
		    .arg(functionFamilyStateName(family->state));
		for (const auto& [binary, occurrence] : family->members)
			text += QString("%1 %2 0x%3 ").arg(binary)
			    .arg(QString::fromStdString(occurrence.name)).arg(occurrence.address, 0, 16);
		return text;
	}

	bool hasInternalGap(int row) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family)
			return false;
		bool seen = false;
		bool gap = false;
		for (const auto& binary : m_binaries)
		{
			bool present = family->members.count(binary.id) != 0;
			if (present && gap)
				return true;
			seen = seen || present;
			gap = gap || (seen && !present);
		}
		return false;
	}

	const std::vector<FunctionFamily>& families() const { return m_families; }

  private:
	std::vector<CorpusBinary> m_binaries;
	std::vector<FunctionFamily> m_families;
	std::map<std::pair<FunctionFamilyId, CorpusBinaryId>, PatchClassification>
	    m_patchClassifications;
};

class CorpusFamilyProxyModel : public QSortFilterProxyModel
{
  public:
	explicit CorpusFamilyProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}

	void setStateFilter(int filter)
	{
		beginFilterChange();
		m_stateFilter = filter;
		endFilterChange(Direction::Rows);
	}
	void setSearch(const QString& search)
	{
		beginFilterChange();
		m_search = search;
		endFilterChange(Direction::Rows);
	}
	void setMinimums(double similarity, double confidence)
	{
		beginFilterChange();
		m_minimumSimilarity = similarity;
		m_minimumConfidence = confidence;
		endFilterChange(Direction::Rows);
	}
	void setPresenceFilters(std::optional<CorpusBinaryId> present,
		std::optional<CorpusBinaryId> absent)
	{
		beginFilterChange();
		m_presentBinary = present;
		m_absentBinary = absent;
		endFilterChange(Direction::Rows);
	}

  protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override
	{
		auto* model = dynamic_cast<CorpusFamilyModel*>(sourceModel());
		if (!model)
			return true;
		const FunctionFamily* family = model->familyAt(sourceRow);
		if (!family)
			return false;
		bool stateAccepted = true;
		switch (m_stateFilter)
		{
		case 1: stateAccepted = family->state == FunctionFamilyState::Changed; break;
		case 2: stateAccepted = family->state == FunctionFamilyState::Missing
		    || family->state == FunctionFamilyState::Unique; break;
		case 3: stateAccepted = family->state == FunctionFamilyState::Ambiguous
		    || family->state == FunctionFamilyState::Conflicting; break;
		case 4: stateAccepted = family->state == FunctionFamilyState::Identical; break;
		case 5: stateAccepted = model->hasInternalGap(sourceRow); break;
		default: break;
		}
		if (!stateAccepted)
			return false;
		if (m_presentBinary && !family->members.count(*m_presentBinary))
			return false;
		if (m_absentBinary && family->members.count(*m_absentBinary))
			return false;
		if (!family->acceptedLinks.empty()
		    && (family->minimumSimilarity < m_minimumSimilarity
		        || family->minimumConfidence < m_minimumConfidence))
			return false;
		return m_search.isEmpty()
		    || model->searchText(sourceRow).contains(m_search, Qt::CaseInsensitive);
	}

  private:
	int m_stateFilter = 0;
	QString m_search;
	double m_minimumSimilarity = 0.0;
	double m_minimumConfidence = 0.0;
	std::optional<CorpusBinaryId> m_presentBinary;
	std::optional<CorpusBinaryId> m_absentBinary;
};

class CorpusLineageModel : public QAbstractTableModel
{
  public:
	CorpusLineageModel(std::vector<CorpusBinary> binaries,
		std::vector<CorpusBinaryId> order, QObject* parent = nullptr) :
		QAbstractTableModel(parent), m_binaries(std::move(binaries)),
		m_order(std::move(order))
	{}

	int rowCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_families.size());
	}
	int columnCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_order.size() + 1);
	}
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override
	{
		if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0)
			return {};
		if (section == 0)
			return "Function family";
		if (static_cast<size_t>(section - 1) >= m_order.size())
			return {};
		CorpusBinaryId binary = m_order[static_cast<size_t>(section - 1)];
		auto found = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [binary](const CorpusBinary& item) { return item.id == binary; });
		return found == m_binaries.end() ? QVariant()
		    : QVariant(QString::fromStdString(found->displayName));
	}
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
	{
		const FunctionFamily* family = familyAt(index.row());
		if (!index.isValid() || !family)
			return {};
		if (role == Qt::UserRole)
			return static_cast<qulonglong>(family->id);
		if (index.column() == 0)
		{
			if (role == Qt::DisplayRole)
			{
				QString name;
				for (CorpusBinaryId binary : m_order)
				{
					auto member = family->members.find(binary);
					if (member != family->members.end())
					{
						name = QString::fromStdString(member->second.name);
						break;
					}
				}
				return QString("%1  %2").arg(family->id).arg(name);
			}
			if (role == Qt::ToolTipRole)
				return QString("Family %1: %2").arg(family->id)
				    .arg(functionFamilyStateName(family->state));
			return {};
		}

		FunctionLineageCell cell = cellAt(index.row(), index.column());
		if (role == Qt::UserRole + 1)
			return static_cast<int>(cell.state);
		if (role == Qt::TextAlignmentRole)
			return Qt::AlignCenter;
		if (role == Qt::DisplayRole)
		{
			switch (cell.state)
			{
			case FunctionLineageState::Absent: return ".";
			case FunctionLineageState::Present: return "P";
			case FunctionLineageState::Introduced: return "+";
			case FunctionLineageState::Reappeared: return "R";
			case FunctionLineageState::Identical: return "=";
			case FunctionLineageState::Changed: return "Changed";
			case FunctionLineageState::VulnerableLike: return "V";
			case FunctionLineageState::FixedLike: return "F";
			case FunctionLineageState::IndependentlyModified: return "M";
			case FunctionLineageState::Ambiguous: return "?";
			case FunctionLineageState::NotAnalyzed: return "NA";
			}
		}
		if (role == Qt::ForegroundRole)
			return QColor(240, 240, 240);
		if (role == Qt::BackgroundRole)
		{
			switch (cell.state)
			{
			case FunctionLineageState::Absent: return QColor(48, 48, 48);
			case FunctionLineageState::Present: return QColor(42, 65, 88);
			case FunctionLineageState::Introduced: return QColor(30, 82, 82);
			case FunctionLineageState::Reappeared: return QColor(70, 48, 92);
			case FunctionLineageState::Identical: return QColor(30, 70, 40);
			case FunctionLineageState::Changed: return QColor(92, 70, 24);
			case FunctionLineageState::VulnerableLike: return QColor(118, 38, 42);
			case FunctionLineageState::FixedLike: return QColor(28, 100, 55);
			case FunctionLineageState::IndependentlyModified: return QColor(38, 64, 105);
			case FunctionLineageState::Ambiguous: return QColor(82, 55, 105);
			case FunctionLineageState::NotAnalyzed: return QColor(72, 72, 72);
			}
		}
		if (role == Qt::ToolTipRole)
		{
			QString text = QString::fromUtf8(functionLineageStateName(cell.state));
			if (cell.occurrence)
				text += QString("\n%1 @ 0x%2")
				    .arg(QString::fromStdString(cell.occurrence->name))
				    .arg(cell.occurrence->address, 0, 16);
			if (cell.similarity)
				text += QString("\nPrevious occurrence similarity: %1%")
				    .arg(*cell.similarity * 100.0, 0, 'f', 1);
			if (cell.confidence)
				text += QString("\nMatch confidence: %1%")
				    .arg(*cell.confidence * 100.0, 0, 'f', 1);
			if (!cell.matchType.empty())
				text += "\nMatch method: " + QString::fromStdString(cell.matchType);
			const PatchClassification* patch = patchAt(family->id,
			    m_order[static_cast<size_t>(index.column() - 1)]);
			if (patch)
			{
				text += QString("\nVulnerable similarity: %1%\nFixed similarity: %2%"
				    "\nScore margin: %3%\nPatch-region coverage: %4%")
				    .arg(patch->vulnerableSimilarity * 100.0, 0, 'f', 1)
				    .arg(patch->fixedSimilarity * 100.0, 0, 'f', 1)
				    .arg(patch->scoreMargin * 100.0, 0, 'f', 1)
				    .arg(patch->patchRegionCoverage * 100.0, 0, 'f', 1);
				for (const auto& evidence : patch->evidence)
					text += "\n" + QString::fromStdString(evidence);
			}
			return text;
		}
		return {};
	}

	void setFamilies(std::vector<FunctionFamily> families)
	{
		beginResetModel();
		m_families = std::move(families);
		endResetModel();
	}
	void setPatchClassifications(FunctionFamilyId family,
		const std::map<CorpusBinaryId, PatchClassification>& classifications)
	{
		for (auto it = m_patchClassifications.begin(); it != m_patchClassifications.end();)
			if (it->first.first == family)
				it = m_patchClassifications.erase(it);
			else
				++it;
		for (const auto& [binary, classification] : classifications)
			m_patchClassifications[{family, binary}] = classification;
		for (size_t row = 0; row < m_families.size(); row++)
			if (m_families[row].id == family && !m_order.empty())
			{
				emit dataChanged(index(static_cast<int>(row), 1),
				    index(static_cast<int>(row), static_cast<int>(m_order.size())));
				break;
			}
	}
	void clearPatchClassifications()
	{
		if (m_patchClassifications.empty())
			return;
		m_patchClassifications.clear();
		if (!m_families.empty() && !m_order.empty())
			emit dataChanged(index(0, 1),
			    index(static_cast<int>(m_families.size() - 1),
			        static_cast<int>(m_order.size())));
	}
	const FunctionFamily* familyAt(int row) const
	{
		return row < 0 || static_cast<size_t>(row) >= m_families.size()
		    ? nullptr : &m_families[static_cast<size_t>(row)];
	}
	FunctionLineageCell cellAt(int row, int column) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family || column <= 0 || static_cast<size_t>(column - 1) >= m_order.size())
			return {};
		CorpusBinaryId binary = m_order[static_cast<size_t>(column - 1)];
		return classifyFunctionLineage(*family, m_order,
		    static_cast<size_t>(column - 1), patchAt(family->id, binary));
	}
	bool hasPatchState(int row) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family)
			return false;
		return std::any_of(m_patchClassifications.begin(), m_patchClassifications.end(),
		    [family](const auto& item) { return item.first.first == family->id; });
	}
	bool hasGap(int row) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family)
			return false;
		bool seen = false;
		bool absentAfterSeen = false;
		for (CorpusBinaryId binary : m_order)
		{
			bool present = family->members.count(binary) != 0;
			if (present && absentAfterSeen)
				return true;
			seen = seen || present;
			absentAfterSeen = absentAfterSeen || (seen && !present);
		}
		return false;
	}
	QString searchText(int row) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family)
			return {};
		QString text = QString::number(family->id) + " ";
		for (const auto& [binary, occurrence] : family->members)
			text += QString("%1 %2 0x%3 ").arg(binary)
			    .arg(QString::fromStdString(occurrence.name)).arg(occurrence.address, 0, 16);
		return text;
	}

  private:
	const PatchClassification* patchAt(FunctionFamilyId family,
		CorpusBinaryId binary) const
	{
		auto found = m_patchClassifications.find({family, binary});
		return found == m_patchClassifications.end() ? nullptr : &found->second;
	}
	std::vector<CorpusBinary> m_binaries;
	std::vector<CorpusBinaryId> m_order;
	std::vector<FunctionFamily> m_families;
	std::map<std::pair<FunctionFamilyId, CorpusBinaryId>, PatchClassification>
	    m_patchClassifications;
};

class CorpusLineageProxyModel : public QSortFilterProxyModel
{
  public:
	explicit CorpusLineageProxyModel(QObject* parent = nullptr) :
		QSortFilterProxyModel(parent)
	{}
	void setViewFilter(int filter)
	{
		beginFilterChange();
		m_filter = filter;
		endFilterChange(Direction::Rows);
	}
	void setSearch(const QString& search)
	{
		beginFilterChange();
		m_search = search;
		endFilterChange(Direction::Rows);
	}

  protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override
	{
		auto* model = dynamic_cast<CorpusLineageModel*>(sourceModel());
		if (!model)
			return true;
		const FunctionFamily* family = model->familyAt(sourceRow);
		if (!family || (!m_search.isEmpty()
		    && !model->searchText(sourceRow).contains(m_search, Qt::CaseInsensitive)))
			return false;
		switch (m_filter)
		{
		case 1: return true;
		case 2: return model->hasPatchState(sourceRow);
		case 3: return model->hasGap(sourceRow);
		default:
			return model->hasPatchState(sourceRow)
			    || family->state != FunctionFamilyState::Identical;
		}
	}

  private:
	int m_filter = 0;
	QString m_search;
};

class CorpusChangesModel : public QAbstractTableModel
{
  public:
	CorpusChangesModel(std::vector<CorpusBinary> binaries,
		std::vector<CorpusBinaryId> order, QObject* parent = nullptr) :
		QAbstractTableModel(parent), m_binaries(std::move(binaries)),
		m_order(std::move(order))
	{}

	int rowCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_families.size());
	}
	int columnCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(std::max<size_t>(1, m_order.size()));
	}
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override
	{
		if (orientation != Qt::Horizontal || section < 0)
			return {};
		if (section == 0)
			return role == Qt::DisplayRole ? QVariant("Function") : QVariant();
		if (static_cast<size_t>(section) >= m_order.size())
			return {};
		QString source = binaryName(m_order[static_cast<size_t>(section - 1)]);
		QString target = binaryName(m_order[static_cast<size_t>(section)]);
		if (role == Qt::DisplayRole)
			return QString("%1\n->\n%2").arg(source, target);
		if (role == Qt::ToolTipRole)
		{
			std::map<FunctionTransitionState, size_t> counts;
			for (size_t row = 0; row < m_families.size(); row++)
				counts[cellAt(static_cast<int>(row), section).state]++;
			QStringList summary;
			auto append = [&summary, &counts](FunctionTransitionState state) {
				auto found = counts.find(state);
				if (found != counts.end() && found->second)
					summary << QString("%1 %2").arg(found->second)
				    .arg(functionTransitionStateName(state));
			};
			append(FunctionTransitionState::Modified);
			append(FunctionTransitionState::Introduced);
			append(FunctionTransitionState::Removed);
			append(FunctionTransitionState::Reappeared);
			append(FunctionTransitionState::PatchBoundary);
			append(FunctionTransitionState::ClassificationChanged);
			append(FunctionTransitionState::Unresolved);
			return QString("%1 -> %2\n%3").arg(source, target,
			    summary.isEmpty() ? QString("No highlighted changes") : summary.join(", "));
		}
		return {};
	}
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
	{
		const FunctionFamily* family = familyAt(index.row());
		if (!index.isValid() || !family)
			return {};
		if (role == Qt::UserRole)
			return static_cast<qulonglong>(family->id);
		if (index.column() == 0)
		{
			QString name;
			for (CorpusBinaryId binary : m_order)
			{
				auto member = family->members.find(binary);
				if (member != family->members.end())
				{
					name = QString::fromStdString(member->second.name);
					break;
				}
			}
			if (role == Qt::DisplayRole)
				return QString("%1  [family %2]").arg(name).arg(family->id);
			if (role == Qt::ToolTipRole)
				return QString("Family %1, present in %2 of %3 builds")
				    .arg(family->id).arg(family->members.size()).arg(m_order.size());
			return {};
		}

		FunctionTransitionCell cell = cellAt(index.row(), index.column());
		if (role == Qt::UserRole + 1)
			return static_cast<int>(cell.state);
		if (role == Qt::TextAlignmentRole)
			return Qt::AlignCenter;
		if (role == Qt::DisplayRole)
		{
			switch (cell.state)
			{
			case FunctionTransitionState::NoOccurrence: return ".";
			case FunctionTransitionState::Unchanged: return "=";
			case FunctionTransitionState::Modified:
				return cell.similarity
				    ? QVariant(QString("Changed · similarity %1%").arg(*cell.similarity * 100.0, 0, 'f', 1))
				    : QVariant("Changed");
			case FunctionTransitionState::Introduced: return "+ Added";
			case FunctionTransitionState::Removed: return "- Removed";
			case FunctionTransitionState::Reappeared: return "R Returned";
			case FunctionTransitionState::Unresolved: return "? Review";
			case FunctionTransitionState::PatchBoundary: return "V -> F";
			case FunctionTransitionState::ClassificationChanged: return "State changed";
			}
		}
		if (role == Qt::ForegroundRole)
			return QColor(244, 244, 244);
		if (role == Qt::BackgroundRole)
		{
			switch (cell.state)
			{
			case FunctionTransitionState::NoOccurrence: return QColor(46, 46, 46);
			case FunctionTransitionState::Unchanged: return QColor(32, 66, 43);
			case FunctionTransitionState::Modified:
			{
				double severity = cell.similarity ? 1.0 - *cell.similarity : 0.5;
				return QColor(104 + static_cast<int>(40.0 * severity), 72, 25);
			}
			case FunctionTransitionState::Introduced: return QColor(25, 91, 68);
			case FunctionTransitionState::Removed: return QColor(120, 43, 46);
			case FunctionTransitionState::Reappeared: return QColor(80, 48, 106);
			case FunctionTransitionState::Unresolved: return QColor(78, 68, 82);
			case FunctionTransitionState::PatchBoundary:
			{
				QLinearGradient split(0, 0, 1, 0);
				split.setCoordinateMode(QGradient::ObjectBoundingMode);
				split.setColorAt(0.0, QColor(124, 39, 43));
				split.setColorAt(0.49, QColor(124, 39, 43));
				split.setColorAt(0.51, QColor(28, 103, 57));
				split.setColorAt(1.0, QColor(28, 103, 57));
				return QBrush(split);
			}
			case FunctionTransitionState::ClassificationChanged:
				return QColor(47, 65, 112);
			}
		}
		if (role == Qt::ToolTipRole)
		{
			QString text = QString::fromUtf8(functionTransitionStateName(cell.state));
			if (cell.source)
				text += QString("\nBefore: %1 @ 0x%2")
				    .arg(QString::fromStdString(cell.source->name))
				    .arg(cell.source->address, 0, 16);
			if (cell.target)
				text += QString("\nAfter: %1 @ 0x%2")
				    .arg(QString::fromStdString(cell.target->name))
				    .arg(cell.target->address, 0, 16);
			if (cell.similarity)
				text += QString("\nSimilarity: %1%")
				    .arg(*cell.similarity * 100.0, 0, 'f', 1);
			if (cell.confidence)
				text += QString("\nConfidence: %1%")
				    .arg(*cell.confidence * 100.0, 0, 'f', 1);
			if (!cell.matchType.empty())
				text += "\nMatch method: " + QString::fromStdString(cell.matchType);
			return text;
		}
		return {};
	}

	void setFamilies(std::vector<FunctionFamily> families)
	{
		beginResetModel();
		m_families = std::move(families);
		endResetModel();
	}
	void setPatchClassifications(FunctionFamilyId family,
		const std::map<CorpusBinaryId, PatchClassification>& classifications)
	{
		for (auto it = m_patchClassifications.begin(); it != m_patchClassifications.end();)
			if (it->first.first == family)
				it = m_patchClassifications.erase(it);
			else
				++it;
		for (const auto& [binary, classification] : classifications)
			m_patchClassifications[{family, binary}] = classification;
		for (size_t row = 0; row < m_families.size(); row++)
			if (m_families[row].id == family && m_order.size() > 1)
			{
				emit dataChanged(index(static_cast<int>(row), 1),
				    index(static_cast<int>(row), static_cast<int>(m_order.size() - 1)));
				emit headerDataChanged(Qt::Horizontal, 1,
				    static_cast<int>(m_order.size() - 1));
				break;
			}
	}
	void clearPatchClassifications()
	{
		if (m_patchClassifications.empty())
			return;
		m_patchClassifications.clear();
		if (!m_families.empty() && m_order.size() > 1)
			emit dataChanged(index(0, 1),
			    index(static_cast<int>(m_families.size() - 1),
			        static_cast<int>(m_order.size() - 1)));
	}
	const FunctionFamily* familyAt(int row) const
	{
		return row < 0 || static_cast<size_t>(row) >= m_families.size()
		    ? nullptr : &m_families[static_cast<size_t>(row)];
	}
	FunctionTransitionCell cellAt(int row, int column) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family || column <= 0 || static_cast<size_t>(column) >= m_order.size())
			return {};
		CorpusBinaryId source = m_order[static_cast<size_t>(column - 1)];
		CorpusBinaryId target = m_order[static_cast<size_t>(column)];
		return classifyFunctionTransition(*family, m_order,
		    static_cast<size_t>(column - 1), patchAt(family->id, source),
		    patchAt(family->id, target));
	}
	bool hasTransition(int row, int filter) const
	{
		for (size_t column = 1; column < m_order.size(); column++)
		{
			FunctionTransitionState state = cellAt(row, static_cast<int>(column)).state;
			if (filter == 1 && state != FunctionTransitionState::NoOccurrence)
				return true;
			if (filter == 2 && (state == FunctionTransitionState::PatchBoundary
			    || state == FunctionTransitionState::ClassificationChanged))
				return true;
			if (filter == 3 && (state == FunctionTransitionState::Introduced
			    || state == FunctionTransitionState::Removed
			    || state == FunctionTransitionState::Reappeared))
				return true;
			if (filter == 4 && state == FunctionTransitionState::Unresolved)
				return true;
			if (filter == 0 && state != FunctionTransitionState::NoOccurrence
			    && state != FunctionTransitionState::Unchanged)
				return true;
		}
		return false;
	}
	QString searchText(int row) const
	{
		const FunctionFamily* family = familyAt(row);
		if (!family)
			return {};
		QString text = QString::number(family->id) + " ";
		for (const auto& [binary, occurrence] : family->members)
			text += QString("%1 %2 0x%3 ").arg(binary)
			    .arg(QString::fromStdString(occurrence.name)).arg(occurrence.address, 0, 16);
		return text;
	}
	const PatchClassification* patchAt(FunctionFamilyId family,
		CorpusBinaryId binary) const
	{
		auto found = m_patchClassifications.find({family, binary});
		return found == m_patchClassifications.end() ? nullptr : &found->second;
	}

  private:
	QString binaryName(CorpusBinaryId id) const
	{
		auto found = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [id](const CorpusBinary& binary) { return binary.id == id; });
		return found == m_binaries.end() ? QString("Binary %1").arg(id + 1)
		                                 : QString::fromStdString(found->displayName);
	}
	std::vector<CorpusBinary> m_binaries;
	std::vector<CorpusBinaryId> m_order;
	std::vector<FunctionFamily> m_families;
	std::map<std::pair<FunctionFamilyId, CorpusBinaryId>, PatchClassification>
	    m_patchClassifications;
};

class CorpusChangesProxyModel : public QSortFilterProxyModel
{
  public:
	explicit CorpusChangesProxyModel(QObject* parent = nullptr) :
		QSortFilterProxyModel(parent)
	{}
	void setViewFilter(int filter)
	{
		beginFilterChange();
		m_filter = filter;
		endFilterChange(Direction::Rows);
	}
	void setSearch(const QString& search)
	{
		beginFilterChange();
		m_search = search;
		endFilterChange(Direction::Rows);
	}

  protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex&) const override
	{
		auto* model = dynamic_cast<CorpusChangesModel*>(sourceModel());
		return !model || (model->hasTransition(sourceRow, m_filter)
		    && (m_search.isEmpty()
		        || model->searchText(sourceRow).contains(m_search, Qt::CaseInsensitive)));
	}

  private:
	int m_filter = 0;
	QString m_search;
};

class CorpusReuseModel : public QAbstractTableModel
{
  public:
	explicit CorpusReuseModel(std::vector<CorpusBinary> binaries,
	    QObject* parent = nullptr) : QAbstractTableModel(parent), m_binaries(std::move(binaries))
	{
		for (const auto& binary : m_binaries)
			m_names[binary.id] = QString::fromStdString(binary.displayName);
	}

	int rowCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
	}
	int columnCount(const QModelIndex& parent = {}) const override
	{
		return parent.isValid() ? 0 : 6;
	}
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override
	{
		if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
			return {};
		static const char* headers[] = {"Source", "Target", "Instruction reuse",
		    "Function coverage", "Mean similarity", "Accepted matches"};
		return section >= 0 && section < 6 ? QVariant(headers[section]) : QVariant();
	}
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
	{
		if (!index.isValid() || index.row() < 0
		    || static_cast<size_t>(index.row()) >= m_rows.size())
			return {};
		const Row& row = m_rows[static_cast<size_t>(index.row())];
		if (role == Qt::TextAlignmentRole && index.column() >= 2)
			return Qt::AlignCenter;
		if (role != Qt::DisplayRole)
			return {};
		switch (index.column())
		{
		case 0: return m_names.value(row.source);
		case 1: return m_names.value(row.target);
		case 2: return QString::number(row.metrics.instructionCoverageSource * 100.0, 'f', 1) + "%";
		case 3: return QString::number(row.metrics.sourceCoverage * 100.0, 'f', 1) + "%";
		case 4: return QString::number(row.metrics.meanSimilarity * 100.0, 'f', 1) + "%";
		case 5: return QString::number(row.metrics.acceptedMatchCount);
		default: return {};
		}
	}

	void setResult(CorpusPairResultRef result)
	{
		if (!result || !result->matches)
			return;
		m_results[result->edge] = result;
		rebuild();
	}

	void invalidateBinary(CorpusBinaryId binary)
	{
		for (auto it = m_results.begin(); it != m_results.end();)
			if (it->first.source == binary || it->first.target == binary)
				it = m_results.erase(it);
			else
				++it;
		rebuild();
	}

	std::optional<CorpusEdgeKey> edgeAt(int row) const
	{
		return row < 0 || static_cast<size_t>(row) >= m_rows.size()
		    ? std::nullopt : std::optional<CorpusEdgeKey>(
		        CorpusEdgeKey {m_rows[static_cast<size_t>(row)].source,
		            m_rows[static_cast<size_t>(row)].target});
	}

  private:
	struct Row
	{
		CorpusBinaryId source;
		CorpusBinaryId target;
		CorpusPairMetrics metrics;
	};
	void rebuild()
	{
		beginResetModel();
		m_rows.clear();
		for (const auto& [edge, result] : m_results)
		{
			m_rows.push_back({edge.source, edge.target, result->metrics});
			m_rows.push_back({edge.target, edge.source, invertCorpusMetrics(result->metrics)});
		}
		std::sort(m_rows.begin(), m_rows.end(), [](const Row& left, const Row& right) {
			if (left.metrics.instructionCoverageSource != right.metrics.instructionCoverageSource)
				return left.metrics.instructionCoverageSource > right.metrics.instructionCoverageSource;
			return std::tie(left.source, left.target) < std::tie(right.source, right.target);
		});
		endResetModel();
	}

	std::vector<CorpusBinary> m_binaries;
	QMap<CorpusBinaryId, QString> m_names;
	std::map<CorpusEdgeKey, CorpusPairResultRef> m_results;
	std::vector<Row> m_rows;
};

CorpusPairEvidence familyEvidence(const CorpusPairResult& result)
{
	CorpusPairEvidence evidence;
	evidence.edge = result.edge;
	if (!result.matches)
		return evidence;
	for (const auto& match : result.matches->matches)
	{
		FunctionOccurrence source {result.edge.source, match.a.address,
		    match.a.name, match.a.instructionCount};
		FunctionOccurrence target {result.edge.target, match.b.address,
		    match.b.name, match.b.instructionCount};
		evidence.links.push_back({source, target, match.similarity,
		    match.confidence, match.matchType, result.edge, match.contentMatch});
	}
	for (const auto& function : result.matches->unmatchedA)
		evidence.unmatchedSource.push_back({result.edge.source, function.address,
		    function.name, function.instructionCount});
	for (const auto& function : result.matches->unmatchedB)
		evidence.unmatchedTarget.push_back({result.edge.target, function.address,
		    function.name, function.instructionCount});
	return evidence;
}

class CorpusWorkspace : public QWidget
{
  public:
	CorpusWorkspace(std::vector<DiffWorkspacePane> panes, CorpusMode mode,
		std::optional<size_t> referenceIndex, bool referenceOverlay,
		bool reuseCachedPairs, QWidget* parent = nullptr) :
		QWidget(parent),
		m_panes(std::move(panes))
	{
		std::vector<CorpusBinaryId> order;
		std::vector<CorpusBinary> metadata;
		CorpusRunRequest request;
		request.reuseCachedPairs = reuseCachedPairs;
		for (size_t index = 0; index < m_panes.size(); index++)
		{
			CorpusBinary binary;
			binary.id = static_cast<CorpusBinaryId>(index);
			QString displayName = QFileInfo(m_panes[index].fileContext->getFilename()).fileName();
			if (displayName.isEmpty())
				displayName = QString("Binary %1").arg(index + 1);
			binary.displayName = displayName.toStdString();
			auto identity = DiffEngine::instance().persistentIdentity(m_panes[index].data);
			FileMetadata* file = m_panes[index].data->GetFile();
			size_t sessionId = file ? file->GetSessionId() : 0;
			binary.persistentIdentity = identity.value_or(
			    "transient:" + std::to_string(sessionId)
			    + ":" + m_panes[index].data->GetTypeName());
			binary.dirty = !identity.has_value();
			binary.functionCount = m_panes[index].data->GetAnalysisFunctionList().size();
			order.push_back(binary.id);
			metadata.push_back(binary);
			request.binaries.push_back({binary, m_panes[index].data});
		}
		std::optional<CorpusBinaryId> reference;
		if (referenceIndex)
			reference = static_cast<CorpusBinaryId>(*referenceIndex);
		request.plan = buildCorpusPlan(mode, order, reference, referenceOverlay);
		m_binaries = metadata;
		m_plan = request.plan;
		CorpusManifest manifestIdentity;
		manifestIdentity.binaries = metadata;
		manifestIdentity.plan = request.plan;
		m_manifestPath = corpusManifestPath(manifestIdentity);
		if (m_manifestPath)
		{
			std::string loadError;
			auto restored = loadCorpusManifest(*m_manifestPath, &loadError);
			if (restored && restored->plan.edges == request.plan.edges)
			{
				m_metricFilters = restored->metricFilters;
				m_familySettings = restored->familySettings;
				m_patchSignature = restored->patchSignature;
				m_patchClassifications = restored->patchClassifications;
				m_patchMinimumMargin = restored->patchMinimumScoreMargin;
				m_patchMinimumCoverage = restored->patchMinimumCoverage;
				m_lastRunElapsedMicroseconds = restored->lastRunElapsedMicroseconds;
				m_lastRunStartingResidentBytes = restored->lastRunStartingResidentBytes;
				m_lastRunPeakResidentBytes = restored->lastRunPeakResidentBytes;
				if (m_patchSignature && !patchSignatureCompatible(*m_patchSignature))
				{
					m_patchSignature.reset();
					m_patchClassifications.clear();
					m_discardedStalePatchSignature = true;
				}
				m_restoredManifest = true;
			}
		}

		auto* layout = new QVBoxLayout(this);
		layout->setContentsMargins(8, 8, 8, 8);
		auto* controls = new QVBoxLayout();
		controls->setContentsMargins(0, 0, 0, 0);
		controls->setSpacing(4);
		auto* progressControls = new QHBoxLayout();
		m_progressText = new QLabel(QString("0/%1 pairs complete").arg(request.plan.edges.size()), this);
		m_progressText->setTextFormat(Qt::PlainText);
		progressControls->addWidget(m_progressText);
		m_progress = new QProgressBar(this);
		m_progress->setRange(0, static_cast<int>(request.plan.edges.size()));
		progressControls->addWidget(m_progress, 1);
		controls->addLayout(progressControls);
		auto* runControls = new QHBoxLayout();
		runControls->addWidget(new QLabel("Matrix metric", this));
		auto* metric = new QComboBox(this);
		metric->addItems({"Source instruction coverage", "Source function coverage",
		    "Mean matched similarity", "Mean confidence", "Accepted matches",
		    "Changed families"});
		configureCompactCombo(metric, 24);
		runControls->addWidget(metric);
		auto* scoreFilterToggle = new QPushButton("Score filters", this);
		scoreFilterToggle->setCheckable(true);
		scoreFilterToggle->setToolTip("Show matrix scoring filters");
		runControls->addWidget(scoreFilterToggle);
		m_cancel = new QPushButton("Cancel", this);
		runControls->addWidget(m_cancel);
		auto* exportJson = new QPushButton("Export JSON", this);
		runControls->addWidget(exportJson);
		auto* exportCsv = new QPushButton("Export CSV", this);
		runControls->addWidget(exportCsv);
		runControls->addStretch(1);
		controls->addLayout(runControls);
		layout->addLayout(controls);

		QString modeName = mode == CorpusMode::Reference ? "Reference vs many"
		    : mode == CorpusMode::Lineage ? "Ordered lineage" : "All pairs";
		auto* direction = new QLabel(QString(
		    "%1. Each cell reports how much of the row binary appears in the column binary. Double-click a completed cell to open the pair workspace.").arg(modeName), this);
		direction->setWordWrap(true);
		layout->addWidget(direction);
		auto* metricFilterPanel = new QWidget(this);
		metricFilterPanel->setVisible(false);
		auto* metricFilters = new QVBoxLayout(metricFilterPanel);
		metricFilters->setContentsMargins(0, 0, 0, 0);
		metricFilters->setSpacing(2);
		auto* primaryMetricFilters = new QHBoxLayout();
		auto* minimumInstructions = new QSpinBox(this);
		minimumInstructions->setRange(0, 1000000);
		minimumInstructions->setPrefix("Min instructions ");
		minimumInstructions->setValue(
		    static_cast<int>(m_metricFilters.minimumInstructionCount));
		primaryMetricFilters->addWidget(minimumInstructions);
		auto* excludeThunks = new QCheckBox("Exclude thunks", this);
		excludeThunks->setChecked(m_metricFilters.excludeThunks);
		primaryMetricFilters->addWidget(excludeThunks);
		auto* includeExact = new QCheckBox("Include exact matches", this);
		includeExact->setChecked(m_metricFilters.includeExactMatches);
		primaryMetricFilters->addWidget(includeExact);
		primaryMetricFilters->addStretch(1);
		metricFilters->addLayout(primaryMetricFilters);
		auto* secondaryMetricFilters = new QHBoxLayout();
		auto* excludeStubs = new QCheckBox("Exclude import and external stubs", this);
		excludeStubs->setChecked(m_metricFilters.excludeExternalStubs);
		secondaryMetricFilters->addWidget(excludeStubs);
		auto* excludeGenerated = new QCheckBox("Exclude generated names with 4 or fewer instructions", this);
		excludeGenerated->setChecked(m_metricFilters.excludeSmallGeneratedFunctions);
		secondaryMetricFilters->addWidget(excludeGenerated);
		secondaryMetricFilters->addStretch(1);
		metricFilters->addLayout(secondaryMetricFilters);
		layout->addWidget(metricFilterPanel);
		connect(scoreFilterToggle, &QPushButton::toggled,
		    metricFilterPanel, &QWidget::setVisible);

		m_model = new CorpusMatrixModel(metadata, request.plan, this);
		m_matrix = new QTableView(this);
		m_matrix->setModel(m_model);
		configureContentSizedColumns(m_matrix);
		m_matrix->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		m_matrix->setSelectionMode(QAbstractItemView::SingleSelection);
		m_matrix->setSelectionBehavior(QAbstractItemView::SelectItems);
		auto* resultTabs = new QTabWidget(this);
		if (mode == CorpusMode::Lineage)
		{
			auto* changesPane = new QWidget(resultTabs);
			auto* changesLayout = new QVBoxLayout(changesPane);
			changesLayout->setContentsMargins(4, 4, 4, 4);
			auto* changesControls = new QHBoxLayout();
			auto* changesFilter = new QComboBox(changesPane);
			changesFilter->addItems({"Changes only", "All transitions",
			    "Patch boundaries", "Added or removed", "Needs review"});
			configureCompactCombo(changesFilter, 18);
			changesControls->addWidget(changesFilter);
			auto* changesSearch = new QLineEdit(changesPane);
			changesSearch->setPlaceholderText("Search function name, family or address");
			changesControls->addWidget(changesSearch, 1);
			changesLayout->addLayout(changesControls);
			auto* changesLegend = new QLabel(
			    "+ added   - removed   Changed modified   = unchanged   R returned   "
			    "V -> F patch boundary   ? needs review", changesPane);
			changesLegend->setWordWrap(true);
			changesLayout->addWidget(changesLegend);

			m_changesModel = new CorpusChangesModel(metadata, order, this);
			m_changesProxy = new CorpusChangesProxyModel(this);
			m_changesProxy->setSourceModel(m_changesModel);
			m_changesProxy->setDynamicSortFilter(true);
			m_changesTable = new QTableView(changesPane);
			m_changesTable->setModel(m_changesProxy);
			m_changesTable->setSelectionBehavior(QAbstractItemView::SelectItems);
			m_changesTable->setSelectionMode(QAbstractItemView::SingleSelection);
			m_changesTable->verticalHeader()->setVisible(false);
			m_changesTable->verticalHeader()->setDefaultSectionSize(26);
			configureContentSizedColumns(m_changesTable);
			m_changesTable->horizontalHeader()->setMinimumHeight(62);
			m_changesTable->setShowGrid(false);
			changesLayout->addWidget(m_changesTable, 1);

			auto* transitionActions = new QHBoxLayout();
			m_openTransition = new QPushButton("Open full diff", changesPane);
			m_openTransition->setEnabled(false);
			transitionActions->addWidget(m_openTransition);
			m_classifyTransition = new QPushButton("Use as patch boundary", changesPane);
			m_classifyTransition->setEnabled(false);
			m_classifyTransition->setToolTip(
			    "Review the before/after direction, then use the functions as vulnerable and fixed anchors");
			transitionActions->addWidget(m_classifyTransition);
			transitionActions->addStretch(1);
			changesLayout->addLayout(transitionActions);
			m_changeEvidence = new QPlainTextEdit(changesPane);
			m_changeEvidence->setReadOnly(true);
			m_changeEvidence->setPlaceholderText(
			    "Select a transition to inspect its function identities and match evidence");
			m_changeEvidence->setMaximumHeight(120);
			changesLayout->addWidget(m_changeEvidence);
			resultTabs->addTab(changesPane, "Changes");
			connect(changesFilter, &QComboBox::currentIndexChanged,
			    m_changesProxy, &CorpusChangesProxyModel::setViewFilter);
			connect(changesSearch, &QLineEdit::textChanged,
			    m_changesProxy, &CorpusChangesProxyModel::setSearch);
		}
		resultTabs->addTab(m_matrix, "Matrix");
		m_reuseModel = new CorpusReuseModel(metadata, this);
		m_reuseTable = new QTableView(resultTabs);
		m_reuseTable->setModel(m_reuseModel);
		configureContentSizedColumns(m_reuseTable);
		m_reuseTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_reuseTable->setSelectionMode(QAbstractItemView::SingleSelection);
		resultTabs->addTab(m_reuseTable, "Reuse ranking");
		auto* lineagePane = new QWidget(resultTabs);
		auto* lineageLayout = new QVBoxLayout(lineagePane);
		lineageLayout->setContentsMargins(4, 4, 4, 4);
		auto* lineageControls = new QHBoxLayout();
		auto* lineageFilter = new QComboBox(lineagePane);
		lineageFilter->addItems({"Changed or classified", "All families",
		    "Patch classified", "Lineage gaps"});
		configureCompactCombo(lineageFilter, 20);
		lineageControls->addWidget(lineageFilter);
		auto* lineageSearch = new QLineEdit(lineagePane);
		lineageSearch->setPlaceholderText("Search function name, family or address");
		lineageControls->addWidget(lineageSearch, 1);
		lineageLayout->addLayout(lineageControls);
		auto* lineageLegend = new QLabel(
		    "P present   + introduced   = identical   Changed modified   R reappeared   "
		    "V vulnerable-like   F fixed-like   M independently modified   ? ambiguous   NA not analyzed",
		    lineagePane);
		lineageLegend->setWordWrap(true);
		lineageLegend->setToolTip(
		    "General lineage states describe function-family matching. Vulnerable-like and fixed-like states use patch-region evidence.");
		lineageLayout->addWidget(lineageLegend);
		m_lineageModel = new CorpusLineageModel(metadata, order, this);
		m_lineageProxy = new CorpusLineageProxyModel(this);
		m_lineageProxy->setSourceModel(m_lineageModel);
		m_lineageProxy->setDynamicSortFilter(true);
		m_lineageTable = new QTableView(lineagePane);
		m_lineageTable->setModel(m_lineageProxy);
		m_lineageTable->setSelectionBehavior(QAbstractItemView::SelectItems);
		m_lineageTable->setSelectionMode(QAbstractItemView::SingleSelection);
		m_lineageTable->verticalHeader()->setVisible(false);
		configureContentSizedColumns(m_lineageTable);
		lineageLayout->addWidget(m_lineageTable, 1);
		resultTabs->addTab(lineagePane, "Lineage");
		connect(lineageFilter, &QComboBox::currentIndexChanged,
		    m_lineageProxy, &CorpusLineageProxyModel::setViewFilter);
		connect(lineageSearch, &QLineEdit::textChanged,
		    m_lineageProxy, &CorpusLineageProxyModel::setSearch);

		auto* familySection = new QWidget(resultTabs);
		auto* familyLayout = new QVBoxLayout(familySection);
		familyLayout->setContentsMargins(0, 6, 0, 0);
		auto* familyControls = new QVBoxLayout();
		familyControls->setContentsMargins(0, 0, 0, 0);
		familyControls->setSpacing(2);
		auto* familyActions = new QHBoxLayout();
		m_familyCount = new QLabel("0 families", familySection);
		familyActions->addWidget(m_familyCount);
		auto* createPatchSignature = new QPushButton("Classify patch state", familySection);
		createPatchSignature->setToolTip(
		    "Select two occurrence cells in one family, then confirm or swap the vulnerable/fixed direction.");
		familyActions->addWidget(createPatchSignature);
		familyActions->addWidget(new QLabel("Margin", familySection));
		auto* patchMargin = new QDoubleSpinBox(familySection);
		patchMargin->setRange(0.0, 100.0);
		patchMargin->setValue(m_patchMinimumMargin * 100.0);
		patchMargin->setSuffix("%");
		familyActions->addWidget(patchMargin);
		familyActions->addWidget(new QLabel("Patch coverage", familySection));
		auto* patchCoverage = new QDoubleSpinBox(familySection);
		patchCoverage->setRange(0.0, 100.0);
		patchCoverage->setValue(m_patchMinimumCoverage * 100.0);
		patchCoverage->setSuffix("%");
		familyActions->addWidget(patchCoverage);
		familyActions->addStretch(1);
		familyControls->addLayout(familyActions);
		auto* familyFilters = new QHBoxLayout();
		auto* familyState = new QComboBox(familySection);
		familyState->addItems({"All families", "Changed only", "Missing or unique",
		    "Ambiguous or conflicting", "Identical only", "Lineage gaps"});
		configureCompactCombo(familyState, 18);
		familyFilters->addWidget(familyState);
		auto* presentIn = new QComboBox(familySection);
		presentIn->addItem("Present in any binary", QVariant());
		auto* absentFrom = new QComboBox(familySection);
		absentFrom->addItem("No absence filter", QVariant());
		for (const auto& binary : metadata)
		{
			presentIn->addItem(QString("Present in %1")
			    .arg(QString::fromStdString(binary.displayName)), binary.id);
			absentFrom->addItem(QString("Absent from %1")
			    .arg(QString::fromStdString(binary.displayName)), binary.id);
		}
		configureCompactCombo(presentIn, 22);
		configureCompactCombo(absentFrom, 22);
		familyFilters->addWidget(presentIn);
		familyFilters->addWidget(absentFrom);
		auto* familySearch = new QLineEdit(familySection);
		familySearch->setPlaceholderText("Search function name or address");
		familyFilters->addWidget(familySearch, 1);
		familyControls->addLayout(familyFilters);
		auto makeMinimum = [familySection](const QString& suffix) {
			auto* spin = new QDoubleSpinBox(familySection);
			spin->setRange(0.0, 100.0);
			spin->setDecimals(1);
			spin->setSuffix(suffix);
			return spin;
		};
		auto* familyThresholds = new QHBoxLayout();
		familyThresholds->addWidget(new QLabel("Min score", familySection));
		auto* minimumScore = makeMinimum("%");
		minimumScore->setValue(m_familySettings.minimumSimilarity * 100.0);
		familyThresholds->addWidget(minimumScore);
		familyThresholds->addWidget(new QLabel("Min confidence", familySection));
		auto* minimumConfidence = makeMinimum("%");
		minimumConfidence->setValue(m_familySettings.minimumConfidence * 100.0);
		familyThresholds->addWidget(minimumConfidence);
		familyThresholds->addStretch(1);
		familyControls->addLayout(familyThresholds);
		familyLayout->addLayout(familyControls);

		m_familyModel = new CorpusFamilyModel(metadata, this);
		m_familyProxy = new CorpusFamilyProxyModel(this);
		m_familyProxy->setSourceModel(m_familyModel);
		m_familyProxy->setDynamicSortFilter(true);
		m_familyTable = new QTableView(familySection);
		m_familyTable->setModel(m_familyProxy);
		m_familyTable->setSortingEnabled(true);
		m_familyTable->setSelectionBehavior(QAbstractItemView::SelectItems);
		m_familyTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
		configureContentSizedColumns(m_familyTable);
		familyLayout->addWidget(m_familyTable, 1);
		m_patchEvidence = new QPlainTextEdit(familySection);
		m_patchEvidence->setReadOnly(true);
		m_patchEvidence->setPlaceholderText("Patch-signature evidence appears here");
		m_patchEvidence->setMaximumHeight(150);
		familyLayout->addWidget(m_patchEvidence);
		resultTabs->addTab(familySection, "Evidence");
		layout->addWidget(resultTabs, 1);

		m_details = new QLabel("Queued", this);
		m_details->setTextFormat(Qt::PlainText);
		m_details->setWordWrap(true);
		layout->addWidget(m_details);

		m_scheduler = new CorpusScheduler(this);
		m_familyRebuildTimer = new QTimer(this);
		m_familyRebuildTimer->setSingleShot(true);
		m_familyRebuildTimer->setInterval(150);
		connect(m_familyRebuildTimer, &QTimer::timeout, this,
		    [this]() { startFamilyRebuild(); });
		m_familyWatcher = new QFutureWatcher<std::shared_ptr<FamilyBuildResult>>(this);
		connect(m_familyWatcher,
		    &QFutureWatcher<std::shared_ptr<FamilyBuildResult>>::finished, this,
		    [this]() { finishFamilyRebuild(); });
		m_order = order;
		m_mode = request.plan.mode;
		m_reference = request.plan.reference;
		connect(metric, &QComboBox::currentIndexChanged, this, [this](int selected) {
			static const MatrixMetric metrics[] = {
			    MatrixMetric::SourceInstructionCoverage,
			    MatrixMetric::SourceFunctionCoverage,
			    MatrixMetric::MeanSimilarity,
			    MatrixMetric::MeanConfidence,
			    MatrixMetric::AcceptedMatches,
			    MatrixMetric::ChangedFamilies};
			if (selected >= 0 && selected < 6)
				m_model->setMetric(metrics[selected]);
		});
		connect(m_cancel, &QPushButton::clicked, m_scheduler, &CorpusScheduler::cancel);
		connect(exportJson, &QPushButton::clicked, this, [this]() { exportResults(true); });
		connect(exportCsv, &QPushButton::clicked, this, [this]() { exportResults(false); });
		connect(createPatchSignature, &QPushButton::clicked, this,
		    [this]() { createSelectedPatchSignature(); });
		connect(patchMargin, &QDoubleSpinBox::valueChanged, this,
		    [this](double value) {
			    m_patchMinimumMargin = value / 100.0;
			    if (m_patchSignature && !m_scheduler->running())
				    beginPatchClassification();
			    saveManifest(m_scheduler && m_scheduler->running());
		    });
		connect(patchCoverage, &QDoubleSpinBox::valueChanged, this,
		    [this](double value) {
			    m_patchMinimumCoverage = value / 100.0;
			    if (m_patchSignature && !m_scheduler->running())
				    beginPatchClassification();
			    saveManifest(m_scheduler && m_scheduler->running());
		    });
		auto applyFilters = [this, minimumInstructions, excludeThunks,
		    excludeStubs, excludeGenerated, includeExact]() {
			CorpusMetricFilters filters;
			filters.minimumInstructionCount = static_cast<size_t>(minimumInstructions->value());
			filters.excludeThunks = excludeThunks->isChecked();
			filters.excludeExternalStubs = excludeStubs->isChecked();
			filters.excludeSmallGeneratedFunctions = excludeGenerated->isChecked();
			filters.includeExactMatches = includeExact->isChecked();
			applyMetricFilters(filters);
		};
		connect(minimumInstructions, &QSpinBox::valueChanged, this,
		    [applyFilters](int) { applyFilters(); });
		connect(excludeThunks, &QCheckBox::toggled, this,
		    [applyFilters](bool) { applyFilters(); });
		connect(excludeStubs, &QCheckBox::toggled, this,
		    [applyFilters](bool) { applyFilters(); });
		connect(excludeGenerated, &QCheckBox::toggled, this,
		    [applyFilters](bool) { applyFilters(); });
		connect(includeExact, &QCheckBox::toggled, this,
		    [applyFilters](bool) { applyFilters(); });
		connect(familyState, &QComboBox::currentIndexChanged,
		    m_familyProxy, &CorpusFamilyProxyModel::setStateFilter);
		auto updatePresence = [this, presentIn, absentFrom]() {
			auto selected = [](QComboBox* combo) -> std::optional<CorpusBinaryId> {
				return combo->currentIndex() <= 0 ? std::nullopt
				    : std::optional<CorpusBinaryId>(
				        static_cast<CorpusBinaryId>(combo->currentData().toUInt()));
			};
			m_familyProxy->setPresenceFilters(selected(presentIn), selected(absentFrom));
		};
		connect(presentIn, &QComboBox::currentIndexChanged, this,
		    [updatePresence](int) { updatePresence(); });
		connect(absentFrom, &QComboBox::currentIndexChanged, this,
		    [updatePresence](int) { updatePresence(); });
		connect(familySearch, &QLineEdit::textChanged,
		    m_familyProxy, &CorpusFamilyProxyModel::setSearch);
		auto updateMinimums = [this, minimumScore, minimumConfidence]() {
			m_familySettings.minimumSimilarity = minimumScore->value() / 100.0;
			m_familySettings.minimumConfidence = minimumConfidence->value() / 100.0;
			m_familyProxy->setMinimums(
			    m_familySettings.minimumSimilarity, m_familySettings.minimumConfidence);
			rebuildFamilies();
			if (m_patchSignature && !m_scheduler->running())
				beginPatchClassification();
			saveManifest(m_scheduler && m_scheduler->running());
		};
		connect(minimumScore, &QDoubleSpinBox::valueChanged, this,
		    [updateMinimums](double) { updateMinimums(); });
		connect(minimumConfidence, &QDoubleSpinBox::valueChanged, this,
		    [updateMinimums](double) { updateMinimums(); });
		connect(m_scheduler, &CorpusScheduler::pairStarted, this,
		    [this](uint32_t source, uint32_t target, int completed, int total, bool cached) {
			if (!m_patchPairRun)
				m_model->setRunning({source, target});
			m_progressText->setText(QString("%1/%2 pairs complete. %3 pair %4 → %5")
			    .arg(completed).arg(total).arg(cached ? "Loading cached" : "Matching")
			    .arg(source + 1).arg(target + 1));
			m_details->setText(cached ? "Phase: loading and validating pair cache"
			                          : "Phase: extracting functions and matching");
		});
		connect(m_scheduler, &CorpusScheduler::pairResultReady, this,
		    [this](CorpusPairResultRef result, int completed, int total) {
			if (m_patchPairRun)
			{
				m_patchPairResults[result->edge] = result;
				m_progress->setValue(completed);
				m_progressText->setText(QString(
				    "Patch comparisons: %1/%2 complete").arg(completed).arg(total));
				if (!result->error.empty())
					m_details->setText(QString::fromStdString(result->error));
				return;
			}
			m_pairResults[result->edge] = result;
			if (result->matches && result->matches->valid)
				updateCorpusBinaryCounts(m_binaries, result->edge, result->rawMetrics);
			auto displayed = filteredPairResult(result);
			m_model->setResult(displayed);
			m_reuseModel->setResult(displayed);
			rebuildFamilies();
			m_progress->setValue(completed);
			m_progressText->setText(QString(
			    "%1/%2 pairs complete. Aggregating metrics").arg(completed).arg(total));
			QString state = result->state == CorpusPairState::Cached ? "cached"
			    : result->state == CorpusPairState::Complete ? "complete"
			    : result->state == CorpusPairState::Failed ? "failed" : "cancelled";
			m_details->setText(QString("Pair %1 → %2: %3%4")
			    .arg(result->edge.source + 1).arg(result->edge.target + 1).arg(state)
			    .arg(result->error.empty() ? QString() : QString(". %1").arg(QString::fromStdString(result->error))));
			saveManifest(completed < total);
		});
		connect(m_scheduler, &CorpusScheduler::runFinished, this,
		    [this](CorpusResultRef result) {
			if (m_patchPairRun)
			{
				m_patchPairRun = false;
				m_cancel->setEnabled(false);
				m_progress->setRange(0, static_cast<int>(m_plan.edges.size()));
				m_progress->setValue(static_cast<int>(m_pairResults.size()));
				m_progressText->setText("Corpus analysis complete");
				classifyPatchSignature();
				saveManifest(false);
				return;
			}
			m_familyRebuildTimer->stop();
			rebuildFamilies();
			if (m_patchSignature)
				classifyPatchSignature();
			if (result)
			{
				m_lastRunElapsedMicroseconds =
				    static_cast<uint64_t>(m_workspaceRunTimer.nsecsElapsed() / 1000);
				m_lastRunStartingResidentBytes = result->startingResidentBytes;
				m_lastRunPeakResidentBytes = std::max(
				    result->peakResidentBytes, currentProcessResidentBytes());
			}
			m_cancel->setEnabled(false);
			m_progressText->setText(result && result->partial
			    ? "Corpus analysis stopped early. Completed pairs remain available."
			    : "Corpus analysis complete");
			if (result)
			{
				double elapsedMilliseconds = m_lastRunElapsedMicroseconds / 1000.0;
				double startingMiB = m_lastRunStartingResidentBytes / (1024.0 * 1024.0);
				double peakMiB = m_lastRunPeakResidentBytes / (1024.0 * 1024.0);
				m_details->setText(QString(
				    "Corpus run: %1 ms. Binary Ninja resident memory: %2 MiB at start, %3 MiB sampled peak.")
				    .arg(elapsedMilliseconds, 0, 'f', 3)
				    .arg(startingMiB, 0, 'f', 1).arg(peakMiB, 0, 'f', 1));
			}
			saveManifest(result && result->partial);
		});
		connect(m_matrix, &QTableView::doubleClicked, this,
		    [this](const QModelIndex& index) { openCell(index); });
		connect(m_matrix, &QTableView::clicked, this,
		    [this](const QModelIndex& index) { showCellDetails(index); });
		connect(m_familyTable, &QTableView::doubleClicked, this,
		    [this](const QModelIndex& index) { openFamilyCell(index); });
		connect(m_reuseTable, &QTableView::doubleClicked, this,
		    [this](const QModelIndex& index) { openReuseRow(index.row()); });
		connect(m_lineageTable, &QTableView::clicked, this,
		    [this](const QModelIndex& index) { showLineageCellDetails(index); });
		connect(m_lineageTable, &QTableView::doubleClicked, this,
		    [this](const QModelIndex& index) { openLineageCell(index); });
		if (m_changesTable)
		{
			connect(m_changesTable, &QTableView::clicked, this,
			    [this](const QModelIndex& index) { showTransitionDetails(index); });
			connect(m_changesTable, &QTableView::doubleClicked, this,
			    [this](const QModelIndex& index) {
				    showTransitionDetails(index);
				    openTransitionCell(m_changesProxy->mapToSource(index));
			    });
			connect(m_openTransition, &QPushButton::clicked, this,
			    [this]() { openTransitionCell(m_selectedTransition); });
			connect(m_classifyTransition, &QPushButton::clicked, this,
			    [this]() { classifySelectedTransition(); });
		}

		m_workspaceRunTimer.start();
		if (!m_scheduler->start(request))
		{
			m_cancel->setEnabled(false);
			m_details->setText(QString::fromStdString(request.plan.error));
		}
		else if (m_discardedStalePatchSignature)
			m_details->setText(
			    "Discarded an incompatible saved patch signature. Recreate the patch boundary after pair loading completes.");
		else if (m_restoredManifest)
			m_details->setText("Restored corpus settings. Pair artifacts are loading from cache.");
		g_corpusWorkspaces.insert(this);
	}

	~CorpusWorkspace() override
	{
		if (m_exportTask) m_exportTask->Cancel();
		g_corpusWorkspaces.erase(this);
	}

	void persistMatchesForFile(FileMetadata* file)
	{
		if (!file)
			return;
		std::optional<size_t> savedIndex;
		for (size_t index = 0; index < m_panes.size(); index++)
		{
			FileMetadata* candidate = m_panes[index].data
			    ? m_panes[index].data->GetFile() : nullptr;
			if (candidate && candidate->GetSessionId() == file->GetSessionId())
			{
				savedIndex = index;
				break;
			}
		}
		if (!savedIndex)
			return;

		// Persisting and refreshing identities hash saved databases in full,
		// which can take seconds for large BNDBs. Do that work off the GUI thread
		// and apply only the newest save's identities.
		std::vector<BinaryViewRef> views;
		for (const auto& pane : m_panes)
			views.push_back(pane.data);
		const size_t saved = *savedIndex;
		const uint64_t generation = ++m_identityGeneration;
		using Identities = std::vector<std::optional<std::string>>;
		auto* watcher = new QFutureWatcher<Identities>(this);
		connect(watcher, &QFutureWatcher<Identities>::finished, this,
		    [this, watcher, generation, saved]() {
			watcher->deleteLater();
			if (generation != m_identityGeneration)
				return;
			Identities identities;
			try
			{
				identities = watcher->result();
			}
			catch (...)
			{
				return;
			}
			applyRefreshedIdentities(identities, saved);
		});
		watcher->setFuture(QtConcurrent::run([views, saved]() {
			DiffEngine& engine = DiffEngine::instance();
			Identities identities;
			try
			{
				engine.persistCachedBinaryExtraction(views[saved]);
				for (size_t other = 0; other < views.size(); other++)
				{
					if (other == saved)
						continue;
					bool persisted = engine.persistCachedFunctionMatches(views[saved], views[other]);
					persisted = engine.persistCachedFunctionMatches(views[other], views[saved])
					    || persisted;
					if (persisted)
						engine.persistCachedBinaryExtraction(views[other]);
				}
			}
			catch (const std::exception& exception)
			{
				LogWarn("binja_diff_view: could not refresh corpus caches after save: %s",
				    exception.what());
			}
			for (const auto& view : views)
				identities.push_back(engine.persistentIdentity(view));
			return identities;
		}));
	}

	void applyRefreshedIdentities(
	    const std::vector<std::optional<std::string>>& identities, size_t savedIndex)
	{
		if (identities.size() != m_binaries.size())
			return;
		for (size_t index = 0; index < m_binaries.size(); index++)
		{
			FileMetadata* metadata = m_panes[index].data->GetFile();
			size_t sessionId = metadata ? metadata->GetSessionId() : 0;
			m_binaries[index].persistentIdentity = identities[index].value_or(
			    "transient:" + std::to_string(sessionId) + ":"
			    + m_panes[index].data->GetTypeName());
			m_binaries[index].dirty = !identities[index].has_value();
		}
		if (m_patchSignature)
		{
			m_patchSignature->vulnerableBinaryIdentity =
			    m_binaries[m_patchSignature->vulnerable.binary].persistentIdentity;
			m_patchSignature->fixedBinaryIdentity =
			    m_binaries[m_patchSignature->fixed.binary].persistentIdentity;
		}
		CorpusManifest identityManifest;
		identityManifest.binaries = m_binaries;
		identityManifest.plan = m_plan;
		m_manifestPath = corpusManifestPath(identityManifest);
		saveManifest(m_scheduler && m_scheduler->running());
		m_details->setText(QString("Refreshed corpus caches after saving %1.")
		    .arg(QString::fromStdString(m_binaries[savedIndex].displayName)));
	}

  private:
	CorpusManifest currentManifest(bool partial) const
	{
		CorpusManifest manifest;
		manifest.binaries = m_binaries;
		manifest.plan = m_plan;
		manifest.metricFilters = m_metricFilters;
		manifest.familySettings = m_familySettings;
		manifest.patchSignature = m_patchSignature;
		manifest.patchClassifications = m_patchClassifications;
		manifest.patchMinimumScoreMargin = m_patchMinimumMargin;
		manifest.patchMinimumCoverage = m_patchMinimumCoverage;
		manifest.lastRunElapsedMicroseconds = m_lastRunElapsedMicroseconds;
		manifest.lastRunStartingResidentBytes = m_lastRunStartingResidentBytes;
		manifest.lastRunPeakResidentBytes = m_lastRunPeakResidentBytes;
		manifest.partial = partial;
		for (const auto& edge : m_plan.edges)
		{
			auto pair = m_pairResults.find(edge);
			manifest.pairStates[edge] = pair == m_pairResults.end() || !pair->second
			    ? CorpusPairState::Queued : pair->second->state;
			if (pair != m_pairResults.end() && pair->second)
				manifest.pairErrors[edge] = pair->second->error;
		}
		return manifest;
	}

	void saveManifest(bool partial)
	{
		if (!m_manifestPath)
			return;
		std::string error;
		if (!saveCorpusManifest(*m_manifestPath, currentManifest(partial), &error))
			m_details->setText(QString("Could not save corpus manifest. %1")
			    .arg(QString::fromStdString(error)));
	}

	void exportResults(bool json)
	{
		if (m_exportTask)
		{
			m_details->setText("A corpus export is already running. Cancel it from the background tasks list if needed.");
			return;
		}
		QString filter = json ? "JSON files (*.json)" : "CSV files (*.csv)";
		QString path = QFileDialog::getSaveFileName(this, "Export corpus results",
		    json ? "binja-diff-corpus.json" : "binja-diff-corpus.csv", filter);
		if (path.isEmpty())
			return;
		if (m_exportTask)
			return; // A nested dialog event may have started an export.
		if (m_familyAppliedGeneration != m_familyGeneration)
		{
			m_details->setText("Families are updating. Export again when the rebuild completes.");
			return;
		}
		CorpusManifest manifest = currentManifest(m_scheduler && m_scheduler->running());
		// Copy the displayed generation once. Workers own values and shared pair
		// references only; later arrivals and window closure cannot change the export.
		using ExportResult = std::pair<bool, std::string>;
		auto* watcher = new QFutureWatcher<ExportResult>(this);
		m_exportTask = new BackgroundTask("Binja Diff: exporting corpus results", true);
		auto task = m_exportTask;
		connect(watcher, &QFutureWatcher<ExportResult>::finished, this, [this, watcher, path]() {
			auto [saved, error] = watcher->result();
			m_exportTask = nullptr;
			watcher->deleteLater();
			m_details->setText(saved ? QString("Exported corpus results to %1").arg(path)
			    : QString("Export stopped. %1").arg(QString::fromStdString(error)));
		});
		m_details->setText(QString("Exporting corpus results to %1…").arg(path));
		watcher->setFuture(QtConcurrent::run([json, path = path.toStdString(),
		    manifest = std::move(manifest), pairs = m_pairResults,
		    families = m_familyModel->families(), task]() -> ExportResult {
			ExportResult result;
			try
			{
				auto cancelled = [task]() { return task->IsCancelled(); };
				result.first = json
				    ? exportCorpusJson(path, manifest, pairs, families, &result.second, cancelled)
				    : exportCorpusCsv(path, manifest, pairs, families, &result.second, cancelled);
			}
			catch (const std::exception& exception) { result = {false, exception.what()}; }
			catch (...) { result = {false, "Unexpected export failure"}; }
			task->Finish();
			return result;
		}));
	}

	const FunctionFamily* patchFamily() const
	{
		if (!m_patchSignature)
			return nullptr;
		for (const auto& family : m_familyModel->families())
		{
			auto vulnerable = family.members.find(m_patchSignature->vulnerable.binary);
			auto fixed = family.members.find(m_patchSignature->fixed.binary);
			if (vulnerable != family.members.end() && fixed != family.members.end()
			    && vulnerable->second.address == m_patchSignature->vulnerable.address
			    && fixed->second.address == m_patchSignature->fixed.address)
				return &family;
		}
		return nullptr;
	}

	void renderPatchEvidence(const FunctionFamily& family)
	{
		if (!m_patchSignature)
			return;
		const PatchSignature& signature = *m_patchSignature;
		QStringList evidenceText;
		evidenceText << QString("Vulnerable anchor: %1 @ 0x%2")
		    .arg(QString::fromStdString(signature.vulnerable.name))
		    .arg(signature.vulnerable.address, 0, 16);
		evidenceText << QString("Fixed anchor: %1 @ 0x%2")
		    .arg(QString::fromStdString(signature.fixed.name))
		    .arg(signature.fixed.address, 0, 16);
		evidenceText << QString("Patch signature IL: %1")
		    .arg(ilDisplayName(signature.viewType));
		evidenceText << QString("Vulnerable patch blocks: %1")
		    .arg(signature.vulnerablePatchBlocks.size());
		for (uint64_t address : signature.vulnerablePatchBlocks)
			evidenceText << QString("  0x%1").arg(address, 0, 16);
		evidenceText << QString("Fixed patch blocks: %1")
		    .arg(signature.fixedPatchBlocks.size());
		for (uint64_t address : signature.fixedPatchBlocks)
			evidenceText << QString("  0x%1").arg(address, 0, 16);
		evidenceText << QString("Changed semantic lines: %1, changed token spans: %2")
		    .arg(signature.changedSemanticLines).arg(signature.changedTokenSpans);
		evidenceText << QString("Security relevance: %1 (%2% evidence score)")
		    .arg(securityRelevanceStateName(signature.securityRelevance.state))
		    .arg(signature.securityRelevance.score * 100.0, 0, 'f', 1);
		for (const auto& value : signature.securityRelevance.evidence)
			evidenceText << "  " + QString::fromStdString(value);
		if (!signature.securityRelevance.counterEvidence.empty())
			evidenceText << "Security counterevidence";
		for (const auto& value : signature.securityRelevance.counterEvidence)
			evidenceText << "  " + QString::fromStdString(value);
		for (const auto& line : signature.changedLines)
		{
			QString vulnerableLocation = line.vulnerableAddress
			    ? QString("0x%1").arg(*line.vulnerableAddress, 0, 16) : QString("-");
			QString fixedLocation = line.fixedAddress
			    ? QString("0x%1").arg(*line.fixedAddress, 0, 16) : QString("-");
			evidenceText << QString("  %1 [%2 -> %3]: %4 -> %5")
			    .arg(QString::fromStdString(line.operation))
			    .arg(vulnerableLocation).arg(fixedLocation)
			    .arg(QString::fromStdString(line.vulnerableText))
			    .arg(QString::fromStdString(line.fixedText));
		}
		auto appendEvidence = [&evidenceText](const QString& label,
		    const std::vector<std::string>& values) {
			if (values.empty())
				return;
			evidenceText << label;
			for (const auto& value : values)
				evidenceText << "  " + QString::fromStdString(value);
		};
		appendEvidence("Changed tokens", signature.changedTokens);
		appendEvidence("Changed call targets", signature.changedCallTargets);
		appendEvidence("Changed constants", signature.changedConstants);
		appendEvidence("Changed data or string references",
		    signature.changedStringReferences);
		for (const auto& [binary, classification] : m_patchClassifications)
		{
			evidenceText << QString("%1: %2")
			    .arg(QString::fromStdString(m_binaries[binary].displayName))
			    .arg(patchStateName(classification.state));
			for (const auto& evidence : classification.evidence)
				evidenceText << "  " + QString::fromStdString(evidence);
		}
		m_patchEvidence->setPlainText(evidenceText.join('\n'));
		m_familyModel->setPatchClassifications(family.id, m_patchClassifications);
		m_lineageModel->setPatchClassifications(family.id, m_patchClassifications);
		if (m_changesModel)
			m_changesModel->setPatchClassifications(family.id, m_patchClassifications);
	}

	void classifyPatchSignature()
	{
		if (!m_patchSignature)
			return;
		if (m_familyAppliedGeneration != m_familyGeneration)
			return; // Reclassification follows publication of the latest families.
		const FunctionFamily* family = patchFamily();
		if (!family)
		{
			m_patchClassifications.clear();
			m_lineageModel->clearPatchClassifications();
			if (m_changesModel)
				m_changesModel->clearPatchClassifications();
			m_patchEvidence->setPlainText(
			    "The anchor occurrences no longer belong to one function family.");
			m_details->setText(
			    "Patch classification is stale because family thresholds separated the anchors.");
			return;
		}
		m_patchSignature->family = family->id;
		auto vulnerablePane = currentlyOpen(m_patchSignature->vulnerable.binary);
		auto fixedPane = currentlyOpen(m_patchSignature->fixed.binary);
		std::map<CorpusBinaryId, PatchClassification> classifications;
		for (CorpusBinaryId binary : m_order)
		{
			auto occurrence = family->members.find(binary);
			std::optional<FunctionOccurrence> target = occurrence == family->members.end()
			    ? std::nullopt : std::optional<FunctionOccurrence>(occurrence->second);
			auto targetPane = currentlyOpen(binary);
			PatchClassification classification = evaluatePatchTarget(*m_patchSignature,
			    vulnerablePane ? vulnerablePane->data : BinaryViewRef {},
			    fixedPane ? fixedPane->data : BinaryViewRef {},
			    targetPane ? targetPane->data : BinaryViewRef {}, target,
			    m_patchMinimumMargin, m_patchMinimumCoverage);
			classification.binary = binary;
			classifications[binary] = std::move(classification);
		}
		m_patchClassifications = std::move(classifications);
		renderPatchEvidence(*family);
		size_t analyzed = std::count_if(m_patchClassifications.begin(),
		    m_patchClassifications.end(), [](const auto& item) {
			    return item.second.state != PatchState::NotAnalyzed;
		    });
		m_details->setText(QString("Patch classification updated: %1/%2 occurrences analyzed.")
		    .arg(analyzed).arg(m_patchClassifications.size()));
	}

	void beginPatchClassification()
	{
		if (!m_patchSignature)
			return;
		if (m_familyAppliedGeneration != m_familyGeneration)
			return;
		if (m_scheduler->running())
		{
			m_details->setText("Wait for the current corpus run before classifying patch state.");
			return;
		}
		const FunctionFamily* family = patchFamily();
		if (!family)
		{
			classifyPatchSignature();
			return;
		}
		std::vector<CorpusEdgeKey> missing;
		for (const CorpusEdgeKey& edge : requiredPatchComparisonEdges(
		    *m_patchSignature, *family))
		{
			if (!DiffEngine::instance().cachedFunctionMatchesRef(
			        m_panes[edge.source].data, m_panes[edge.target].data))
				missing.push_back(edge);
		}
		if (missing.empty())
		{
			classifyPatchSignature();
			saveManifest(false);
			return;
		}

		CorpusRunRequest request;
		request.reuseCachedPairs = true;
		for (size_t index = 0; index < m_panes.size(); index++)
			request.binaries.push_back({m_binaries[index], m_panes[index].data});
		request.plan.mode = m_mode;
		request.plan.binaryOrder = m_order;
		request.plan.edges = missing;
		request.plan.valid = true;
		m_patchPairResults.clear();
		m_patchPairRun = true;
		m_progress->setRange(0, static_cast<int>(request.plan.edges.size()));
		m_progress->setValue(0);
		m_progressText->setText(QString("Scheduling %1 missing patch comparisons")
		    .arg(request.plan.edges.size()));
		m_cancel->setEnabled(true);
		if (!m_scheduler->start(request))
		{
			m_patchPairRun = false;
			m_cancel->setEnabled(false);
			m_details->setText("Could not start the missing patch comparisons.");
		}
	}

	void createSelectedPatchSignature()
	{
		QModelIndexList selected = m_familyTable->selectionModel()->selectedIndexes();
		std::vector<std::pair<QModelIndex, FunctionOccurrence>> occurrences;
		for (const QModelIndex& proxyIndex : selected)
		{
			QModelIndex sourceIndex = m_familyProxy->mapToSource(proxyIndex);
			auto occurrence = m_familyModel->occurrenceAt(
			    sourceIndex.row(), sourceIndex.column());
			if (occurrence)
				occurrences.push_back({sourceIndex, *occurrence});
		}
		if (occurrences.size() != 2 || occurrences[0].first.row() != occurrences[1].first.row())
		{
			m_details->setText(
			    "Select exactly two function occurrence cells from one family.");
			return;
		}
		auto orderPosition = [this](CorpusBinaryId binary) {
			auto found = std::find(m_order.begin(), m_order.end(), binary);
			return found == m_order.end() ? m_order.size()
			                              : static_cast<size_t>(std::distance(m_order.begin(), found));
		};
		if (orderPosition(occurrences[1].second.binary)
		    < orderPosition(occurrences[0].second.binary))
			std::swap(occurrences[0], occurrences[1]);
		const FunctionOccurrence& vulnerable = occurrences[0].second;
		const FunctionOccurrence& fixed = occurrences[1].second;
		const FunctionFamily* family = m_familyModel->familyAt(occurrences[0].first.row());
		if (!family)
			return;
		createPatchSignatureFrom(*family, vulnerable, fixed);
	}

	void createPatchSignatureFrom(FunctionFamily family,
		FunctionOccurrence vulnerable, FunctionOccurrence fixed)
	{
		if (m_familyAppliedGeneration != m_familyGeneration)
		{
			m_details->setText("Families are updating. Select patch anchors after the rebuild completes.");
			return;
		}
		const uint64_t generation = m_familyGeneration;
		const FunctionOccurrence* vulnerableAnchor = &vulnerable;
		const FunctionOccurrence* fixedAnchor = &fixed;
		QString question = QString(
		    "The security model needs an explicit direction.\n\n"
		    "Vulnerable anchor: %1 @ 0x%2\nFixed anchor: %3 @ 0x%4")
		    .arg(QString::fromStdString(vulnerable.name)).arg(vulnerable.address, 0, 16)
		    .arg(QString::fromStdString(fixed.name)).arg(fixed.address, 0, 16);
		QMessageBox confirmation(QMessageBox::Question, "Create patch signature",
		    question, QMessageBox::NoButton, this);
		confirmation.setTextFormat(Qt::PlainText);
		QPushButton* useDirection = confirmation.addButton(
		    "Use this direction", QMessageBox::AcceptRole);
		QPushButton* swapDirection = confirmation.addButton(
		    "Swap vulnerable/fixed", QMessageBox::ActionRole);
		confirmation.addButton(QMessageBox::Cancel);
		confirmation.exec();
		if (generation != m_familyGeneration)
		{
			m_details->setText("Families changed while confirming anchors. Select the anchors again.");
			return;
		}
		if (confirmation.clickedButton() == swapDirection)
			std::swap(vulnerableAnchor, fixedAnchor);
		else if (confirmation.clickedButton() != useDirection)
			return;
		auto vulnerablePane = currentlyOpen(vulnerableAnchor->binary);
		auto fixedPane = currentlyOpen(fixedAnchor->binary);
		if (!vulnerablePane || !fixedPane)
		{
			m_details->setText("Reopen both anchor tabs before creating a patch signature.");
			return;
		}
		std::string error;
		auto signature = buildPatchSignature(family.id,
		    vulnerablePane->data, *vulnerableAnchor,
		    fixedPane->data, *fixedAnchor, &error);
		if (!signature)
		{
			m_details->setText(QString::fromStdString(error));
			return;
		}
		m_patchSignature = *signature;
		m_patchClassifications.clear();
		m_lineageModel->clearPatchClassifications();
		if (m_changesModel)
			m_changesModel->clearPatchClassifications();
		beginPatchClassification();
	}

	CorpusPairResultRef filteredPairResult(CorpusPairResultRef result,
	    CorpusMetricFlagBatch* batch = nullptr) const
	{
		if (!result || !result->matches
		    || static_cast<size_t>(result->edge.source) >= m_panes.size()
		    || static_cast<size_t>(result->edge.target) >= m_panes.size())
			return result;
		auto filtered = std::make_shared<CorpusPairResult>(*result);
		filtered->metricFilters = m_metricFilters;
		bool defaultFilters = m_metricFilters.minimumInstructionCount == 0
		    && !m_metricFilters.excludeThunks
		    && !m_metricFilters.excludeExternalStubs
		    && !m_metricFilters.excludeSmallGeneratedFunctions
		    && m_metricFilters.generatedFunctionMaximumInstructions == 4
		    && m_metricFilters.includeExactMatches;
		if (defaultFilters) filtered->metrics = result->rawMetrics;
		else
		{
			CorpusMetricFlagBatch local(m_metricFilters);
			auto& flags = batch ? *batch : local;
			auto capture = [&](CorpusBinaryId binary) {
				return captureCorpusFunctionFlags(m_panes[static_cast<size_t>(binary)].data);
			};
			const auto& source = flags.get(result->edge.source, [&]() { return capture(result->edge.source); });
			const auto& target = flags.get(result->edge.target, [&]() { return capture(result->edge.target); });
			filtered->metrics = calculateCorpusPairMetricsWithFlags(
			    *result->matches, source, target, m_metricFilters);
		}
		return filtered;
	}

	void applyMetricFilters(const CorpusMetricFilters& filters)
	{
		m_metricFilters = filters;
		CorpusMetricFlagBatch flags(filters);
		for (const auto& [edge, result] : m_pairResults)
		{
			(void)edge;
			auto filtered = filteredPairResult(result, &flags);
			m_model->setResult(filtered);
			m_reuseModel->setResult(filtered);
		}
		showCellDetails(m_matrix->currentIndex());
		saveManifest(m_scheduler && m_scheduler->running());
	}

	void rebuildFamilies()
	{
		// Invalidate the in-flight result immediately, including during debounce.
		++m_familyGeneration;
		m_familyRebuildTimer->start();
	}

	void startFamilyRebuild()
	{
		// Keep at most one job alive. Its completion starts the newest request.
		if (m_familyWorkerRunning)
			return;
		m_familyWorkerRunning = true;
		m_familyWorkerGeneration = m_familyGeneration;
		if (m_scheduler && m_scheduler->running() && !m_patchPairRun)
			m_progressText->setText("Phase: rebuilding function families");
		// Only immutable pair payloads and value settings cross into the worker.
		// No QWidget, model, BinaryView, or workspace lifetime is captured.
		m_familyWatcher->setFuture(QtConcurrent::run(
		    [pairs = m_pairResults, mode = m_mode, reference = m_reference,
		        order = m_order, settings = m_familySettings]() {
			    auto result = std::make_shared<FamilyBuildResult>();
			    QElapsedTimer timer;
			    timer.start();
			    try
			    {
				    std::vector<CorpusPairEvidence> evidence;
				    for (const auto& [edge, pair] : pairs)
				    {
					    (void)edge;
					    if (pair && pair->matches)
						    evidence.push_back(familyEvidence(*pair));
				    }
				    result->families = mode == CorpusMode::Reference && reference
				        ? buildReferenceFamilies(*reference, order, evidence, settings)
				        : buildConstrainedFamilies(mode, order, evidence, settings);
			    }
			    catch (const std::exception& exception)
			    {
				    result->error = exception.what();
			    }
			    catch (...)
			    {
				    result->error = "Unknown family rebuild error";
			    }
			    result->elapsedMilliseconds = timer.elapsed();
			    return result;
		    }));
	}

	void finishFamilyRebuild()
	{
		m_familyWorkerRunning = false;
		if (m_familyWorkerGeneration != m_familyGeneration)
		{
			if (!m_familyRebuildTimer->isActive())
				startFamilyRebuild();
			return;
		}
		auto result = m_familyWatcher->result();
		if (!result->error.empty())
		{
			m_details->setText("Could not rebuild families: "
			    + QString::fromStdString(result->error));
			return;
		}
		m_familyAppliedGeneration = m_familyWorkerGeneration;
		auto families = std::move(result->families);
		// Model resets invalidate QModelIndex values. Remember occurrence identity,
		// including the selected binary column when it contains an occurrence.
		using SelectionKey = std::tuple<CorpusBinaryId, uint64_t, int>;
		struct SavedSelection
		{
			QTableView* table;
			QSortFilterProxyModel* proxy;
			std::vector<SelectionKey> cells;
			std::optional<SelectionKey> current;
			int scroll;
		};
		auto saveSelection = [](QTableView* table, QSortFilterProxyModel* proxy,
		    auto* model, int binaryColumnOffset, const std::vector<CorpusBinaryId>& columns) {
			SavedSelection saved {table, proxy, {}, {}, table->verticalScrollBar()->value()};
			auto key = [&](const QModelIndex& index) -> std::optional<SelectionKey> {
				if (!index.isValid())
					return {};
				auto source = proxy->mapToSource(index);
				const FunctionFamily* family = model->familyAt(source.row());
				if (!family || family->members.empty())
					return {};
				auto member = family->members.begin();
				int position = source.column() - binaryColumnOffset;
				if (position >= 0 && static_cast<size_t>(position) < columns.size())
				{
					auto atColumn = family->members.find(columns[position]);
					if (atColumn != family->members.end())
						member = atColumn;
				}
				return SelectionKey {member->first, member->second.address, source.column()};
			};
			for (const auto& index : table->selectionModel()->selectedIndexes())
				if (auto cell = key(index))
					saved.cells.push_back(*cell);
			saved.current = key(table->currentIndex());
			return saved;
		};
		std::vector<SavedSelection> selections;
		std::vector<CorpusBinaryId> familyColumns;
		for (const auto& binary : m_binaries)
			familyColumns.push_back(binary.id);
		selections.push_back(saveSelection(m_familyTable, m_familyProxy, m_familyModel, 2, familyColumns));
		selections.push_back(saveSelection(m_lineageTable, m_lineageProxy, m_lineageModel, 1, m_order));
		if (m_changesTable)
			selections.push_back(saveSelection(m_changesTable, m_changesProxy, m_changesModel, 1, m_order));
		std::map<std::pair<CorpusBinaryId, uint64_t>, int> rows;
		for (size_t row = 0; row < families.size(); ++row)
			for (const auto& [binary, member] : families[row].members)
				rows[{binary, member.address}] = static_cast<int>(row);
		m_familyCount->setText(QString("%1 families").arg(families.size()));
		m_lineageModel->setFamilies(families);
		if (m_changesModel)
			m_changesModel->setFamilies(families);
		m_familyModel->setFamilies(std::move(families));
		m_model->setFamilies(m_familyModel->families());
		for (const auto& saved : selections)
		{
			auto indexFor = [&](const SelectionKey& key) {
				auto [binary, address, column] = key;
				auto row = rows.find({binary, address});
				return row == rows.end() ? QModelIndex {}
				    : saved.proxy->mapFromSource(saved.proxy->sourceModel()->index(row->second, column));
			};
			QItemSelection selection;
			for (const auto& cell : saved.cells)
			{
				auto index = indexFor(cell);
				if (index.isValid())
					selection.select(index, index);
			}
			saved.table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
			if (saved.current)
				saved.table->selectionModel()->setCurrentIndex(indexFor(*saved.current), QItemSelectionModel::NoUpdate);
			saved.table->verticalScrollBar()->setValue(saved.scroll);
		}
		if (m_patchSignature && !m_patchClassifications.empty())
			if (const FunctionFamily* family = patchFamily())
			{
				m_patchSignature->family = family->id;
				m_familyModel->setPatchClassifications(
				    family->id, m_patchClassifications);
				m_lineageModel->setPatchClassifications(
				    family->id, m_patchClassifications);
				if (m_changesModel)
					m_changesModel->setPatchClassifications(
					    family->id, m_patchClassifications);
			}
		LogInfo("binja_diff_view: rebuilt %zu corpus families in %lld ms",
		    m_familyModel->families().size(),
		    static_cast<long long>(result->elapsedMilliseconds));
		if (m_patchSignature && !m_scheduler->running())
			beginPatchClassification();
		if (m_changesTable)
		{
			if (m_changesTable->currentIndex().isValid())
				showTransitionDetails(m_changesTable->currentIndex());
			else
			{
				m_selectedTransition = QPersistentModelIndex {};
				m_openTransition->setEnabled(false);
				m_classifyTransition->setEnabled(false);
				m_changeEvidence->clear();
			}
		}
	}

	std::optional<DiffWorkspacePane> currentlyOpen(CorpusBinaryId binary) const
	{
		if (static_cast<size_t>(binary) >= m_panes.size())
			return std::nullopt;
		std::vector<DiffWorkspacePane> current = collectOpenDiffPanes(UIContext::activeContext());
		BinaryViewRef expected = m_panes[static_cast<size_t>(binary)].data;
		auto found = std::find_if(current.begin(), current.end(),
		    [expected](const DiffWorkspacePane& pane) {
			    return pane.data && expected && pane.data->GetObject() == expected->GetObject();
		    });
		return found == current.end() ? std::nullopt
		                              : std::optional<DiffWorkspacePane>(*found);
	}

	std::optional<PreparedFunctionMatches> preparedCorpusPair(
	    CorpusBinaryId source, CorpusBinaryId target,
	    const FunctionMatchMapsRef& expected = {}) const
	{
		if (static_cast<size_t>(source) >= m_panes.size()
		    || static_cast<size_t>(target) >= m_panes.size())
			return std::nullopt;
		auto findResult = [source, target](
		    const std::map<CorpusEdgeKey, CorpusPairResultRef>& results) {
			auto found = results.find({source, target});
			if (found == results.end())
				found = results.find({target, source});
			return found == results.end() ? CorpusPairResultRef {} : found->second;
		};
		CorpusPairResultRef result = findResult(m_pairResults);
		if (!result)
			result = findResult(m_patchPairResults);
		if (!result || !result->matches
		    || (expected && expected.get() != result->matches.get())
		    || static_cast<size_t>(result->edge.source) >= m_panes.size()
		    || static_cast<size_t>(result->edge.target) >= m_panes.size())
			return std::nullopt;

		DiffEngine& engine = DiffEngine::instance();
		auto current = engine.cachedFunctionMatchesRef(
		    m_panes[result->edge.source].data, m_panes[result->edge.target].data);
		if (!current || current.get() != result->matches.get())
			return std::nullopt;
		return engine.prepareCachedFunctionMatches(
		    m_panes[source].data, m_panes[target].data);
	}

	void openOccurrencePair(const FunctionOccurrence& source,
		const FunctionOccurrence& target)
	{
		auto openSource = currentlyOpen(source.binary);
		auto openTarget = currentlyOpen(target.binary);
		if (!openSource || !openTarget)
		{
			m_details->setText("Reopen both source tabs before drilling into this family.");
			return;
		}
		openSource->offset = source.address;
		openTarget->offset = target.address;
		auto prepared = preparedCorpusPair(source.binary, target.binary);
		if (!prepared)
		{
			m_details->setText(
			    "This family pair is no longer the corpus snapshot. Open a new corpus workspace.");
			return;
		}
		std::optional<FunctionViewType> preferredViewType;
		if (m_patchSignature)
			if (const FunctionFamily* family = patchFamily())
			{
				auto sourceMember = family->members.find(source.binary);
				auto targetMember = family->members.find(target.binary);
				if (sourceMember != family->members.end()
				    && targetMember != family->members.end()
				    && sourceMember->second.address == source.address
				    && targetMember->second.address == target.address)
					preferredViewType = m_patchSignature->viewType;
			}
		openPairWorkspace(UIContext::activeContext(), *openSource, *openTarget,
		    std::move(prepared), true, std::move(preferredViewType));
	}

	void openFamilyOccurrence(const FunctionFamily& family,
		const FunctionOccurrence& occurrence)
	{
		if (family.members.size() < 2)
			return;
		std::optional<FunctionOccurrence> anchor;
		if (m_mode == CorpusMode::Reference && m_reference
		    && occurrence.binary != *m_reference)
		{
			auto found = family.members.find(*m_reference);
			if (found != family.members.end())
				anchor = found->second;
		}
		else
		{
			auto position = std::find(m_order.begin(), m_order.end(), occurrence.binary);
			while (position != m_order.begin() && position != m_order.end())
			{
				--position;
				auto found = family.members.find(*position);
				if (found != family.members.end())
				{
					anchor = found->second;
					break;
				}
			}
		}
		if (!anchor)
			for (const auto& [binary, member] : family.members)
				if (binary != occurrence.binary)
				{
					anchor = member;
					break;
				}
		if (anchor)
			openOccurrencePair(*anchor, occurrence);
	}

	void openFamilyCell(const QModelIndex& proxyIndex)
	{
		if (!proxyIndex.isValid())
			return;
		QModelIndex sourceIndex = m_familyProxy->mapToSource(proxyIndex);
		auto occurrence = m_familyModel->occurrenceAt(
		    sourceIndex.row(), sourceIndex.column());
		const FunctionFamily* family = m_familyModel->familyAt(sourceIndex.row());
		if (!occurrence || !family)
			return;
		openFamilyOccurrence(*family, *occurrence);
	}

	void openLineageCell(const QModelIndex& proxyIndex)
	{
		if (!proxyIndex.isValid() || proxyIndex.column() <= 0)
			return;
		QModelIndex sourceIndex = m_lineageProxy->mapToSource(proxyIndex);
		const FunctionFamily* family = m_lineageModel->familyAt(sourceIndex.row());
		FunctionLineageCell cell = m_lineageModel->cellAt(
		    sourceIndex.row(), sourceIndex.column());
		if (family && cell.occurrence)
			openFamilyOccurrence(*family, *cell.occurrence);
	}

	QString corpusBinaryName(CorpusBinaryId binary) const
	{
		auto found = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [binary](const CorpusBinary& item) { return item.id == binary; });
		return found == m_binaries.end() ? QString("Binary %1").arg(binary + 1)
		                                  : QString::fromStdString(found->displayName);
	}

	void showTransitionDetails(const QModelIndex& proxyIndex)
	{
		if (!m_changesModel || !m_changesProxy || !proxyIndex.isValid())
			return;
		QModelIndex sourceIndex = m_changesProxy->mapToSource(proxyIndex);
		m_selectedTransition = QPersistentModelIndex(sourceIndex);
		const FunctionFamily* family = m_changesModel->familyAt(sourceIndex.row());
		if (!family)
			return;
		if (sourceIndex.column() == 0)
		{
			m_changeEvidence->setPlainText(QString(
			    "Family %1\nPresent in %2 of %3 builds\nSelect a transition cell to inspect the change.")
			    .arg(family->id).arg(family->members.size()).arg(m_order.size()));
			m_openTransition->setEnabled(false);
			m_classifyTransition->setEnabled(false);
			return;
		}

		FunctionTransitionCell cell = m_changesModel->cellAt(
		    sourceIndex.row(), sourceIndex.column());
		CorpusBinaryId sourceBinary = m_order[static_cast<size_t>(sourceIndex.column() - 1)];
		CorpusBinaryId targetBinary = m_order[static_cast<size_t>(sourceIndex.column())];
		QStringList evidence;
		evidence << QString("%1 -> %2")
		    .arg(corpusBinaryName(sourceBinary), corpusBinaryName(targetBinary));
		evidence << QString("Family %1, %2")
		    .arg(family->id).arg(functionTransitionStateName(cell.state));
		if (cell.source)
			evidence << QString("Before: %1 @ 0x%2, %3 instructions")
			    .arg(QString::fromStdString(cell.source->name))
			    .arg(cell.source->address, 0, 16).arg(cell.source->instructionCount);
		else
			evidence << "Before: function absent";
		if (cell.target)
			evidence << QString("After: %1 @ 0x%2, %3 instructions")
			    .arg(QString::fromStdString(cell.target->name))
			    .arg(cell.target->address, 0, 16).arg(cell.target->instructionCount);
		else
			evidence << "After: function absent";
		if (cell.similarity)
			evidence << QString("Similarity %1%, confidence %2%, method %3")
			    .arg(*cell.similarity * 100.0, 0, 'f', 1)
			    .arg(cell.confidence.value_or(0.0) * 100.0, 0, 'f', 1)
			    .arg(cell.matchType.empty() ? "unknown"
			                                : QString::fromStdString(cell.matchType));
		auto appendPatch = [&evidence](const QString& label,
		    const PatchClassification* patch) {
			if (!patch)
				return;
			evidence << QString("%1 patch state: %2, margin %3%, region coverage %4%")
			    .arg(label).arg(patchStateName(patch->state))
			    .arg(patch->scoreMargin * 100.0, 0, 'f', 1)
			    .arg(patch->patchRegionCoverage * 100.0, 0, 'f', 1);
		};
		appendPatch("Before", m_changesModel->patchAt(family->id, sourceBinary));
		appendPatch("After", m_changesModel->patchAt(family->id, targetBinary));
		m_changeEvidence->setPlainText(evidence.join('\n'));
		m_openTransition->setEnabled(cell.source.has_value() || cell.target.has_value());
		m_classifyTransition->setEnabled(
		    cell.source.has_value() && cell.target.has_value());
		m_details->setText(QString("Selected %1 -> %2, family %3: %4.")
		    .arg(corpusBinaryName(sourceBinary), corpusBinaryName(targetBinary))
		    .arg(family->id).arg(functionTransitionStateName(cell.state)));
	}

	void openTransitionCell(const QModelIndex& sourceIndex)
	{
		if (!m_changesModel || !sourceIndex.isValid() || sourceIndex.column() <= 0)
			return;
		FunctionTransitionCell cell = m_changesModel->cellAt(
		    sourceIndex.row(), sourceIndex.column());
		if (cell.source && cell.target)
		{
			openOccurrencePair(*cell.source, *cell.target);
			return;
		}
		CorpusBinaryId sourceBinary = m_order[static_cast<size_t>(sourceIndex.column() - 1)];
		CorpusBinaryId targetBinary = m_order[static_cast<size_t>(sourceIndex.column())];
		auto source = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [sourceBinary](const CorpusBinary& item) { return item.id == sourceBinary; });
		auto target = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [targetBinary](const CorpusBinary& item) { return item.id == targetBinary; });
		if (source != m_binaries.end() && target != m_binaries.end())
			openCell(m_model->index(static_cast<int>(std::distance(m_binaries.begin(), source)),
			    static_cast<int>(std::distance(m_binaries.begin(), target))));
	}

	void classifySelectedTransition()
	{
		if (!m_changesModel || !m_selectedTransition.isValid())
			return;
		if (m_scheduler->running())
		{
			m_details->setText("Wait for the current corpus run before classifying a patch boundary.");
			return;
		}
		QModelIndex sourceIndex = m_selectedTransition;
		const FunctionFamily* family = m_changesModel->familyAt(sourceIndex.row());
		FunctionTransitionCell cell = m_changesModel->cellAt(
		    sourceIndex.row(), sourceIndex.column());
		if (family && cell.source && cell.target)
			createPatchSignatureFrom(*family, *cell.source, *cell.target);
	}

	void showLineageCellDetails(const QModelIndex& proxyIndex)
	{
		if (!proxyIndex.isValid())
			return;
		QModelIndex sourceIndex = m_lineageProxy->mapToSource(proxyIndex);
		const FunctionFamily* family = m_lineageModel->familyAt(sourceIndex.row());
		if (!family)
			return;
		if (sourceIndex.column() == 0)
		{
			m_details->setText(QString("Family %1. %2. Present in %3 of %4 binaries.")
			    .arg(family->id).arg(functionFamilyStateName(family->state))
			    .arg(family->members.size()).arg(m_order.size()));
			return;
		}
		FunctionLineageCell cell = m_lineageModel->cellAt(
		    sourceIndex.row(), sourceIndex.column());
		CorpusBinaryId binary = m_order[static_cast<size_t>(sourceIndex.column() - 1)];
		auto metadata = std::find_if(m_binaries.begin(), m_binaries.end(),
		    [binary](const CorpusBinary& item) { return item.id == binary; });
		QString name = metadata == m_binaries.end() ? QString("Binary %1").arg(binary + 1)
		    : QString::fromStdString(metadata->displayName);
		QString text = QString("%1. Family %2 is %3")
		    .arg(name).arg(family->id).arg(functionLineageStateName(cell.state));
		if (cell.occurrence)
			text += QString(" as %1 @ 0x%2")
			    .arg(QString::fromStdString(cell.occurrence->name))
			    .arg(cell.occurrence->address, 0, 16);
		if (cell.similarity)
			text += QString(". Previous occurrence similarity %1%")
			    .arg(*cell.similarity * 100.0, 0, 'f', 1);
		if (cell.confidence)
			text += QString(", confidence %1%")
			    .arg(*cell.confidence * 100.0, 0, 'f', 1);
		text += ".";
		m_details->setText(text);
	}

	void openCell(const QModelIndex& index)
	{
		if (!index.isValid() || index.row() == index.column())
			return;
		bool inverted = false;
		CorpusPairResultRef pair = m_model->pairAt(
		    index.row(), index.column(), &inverted);
		if (!pair || !pair->matches)
			return;
		size_t row = static_cast<size_t>(index.row());
		size_t column = static_cast<size_t>(index.column());
		std::vector<DiffWorkspacePane> current = collectOpenDiffPanes(UIContext::activeContext());
		auto findOpen = [&current](BinaryViewRef view) -> std::optional<DiffWorkspacePane> {
			auto found = std::find_if(current.begin(), current.end(),
			    [view](const DiffWorkspacePane& pane) {
				    return pane.data && view && pane.data->GetObject() == view->GetObject();
			    });
			return found == current.end() ? std::nullopt
			                              : std::optional<DiffWorkspacePane>(*found);
		};
		auto openRow = findOpen(m_panes[row].data);
		auto openColumn = findOpen(m_panes[column].data);
		if (!openRow || !openColumn)
		{
			m_details->setText(
			    "Reopen both source tabs before drilling into this completed pair.");
			return;
		}
		if (inverted)
		{
			// The matrix may invert matching metrics, but security assessment is
			// directional. Open a normal workspace so B->A gets its own assessment
			// instead of reusing the A->B corpus snapshot.
			openPairWorkspace(UIContext::activeContext(), *openRow, *openColumn);
			return;
		}
		auto prepared = preparedCorpusPair(
		    m_binaries[row].id, m_binaries[column].id,
		    pair->matches);
		if (!prepared)
		{
			m_details->setText(
			    "This pair changed after the corpus run. Open a new corpus workspace before drilling down.");
			return;
		}
		openPairWorkspace(UIContext::activeContext(), *openRow, *openColumn,
		    std::move(prepared), true);
	}

	void showCellDetails(const QModelIndex& index)
	{
		if (!index.isValid() || index.row() == index.column())
			return;
		bool inverted = false;
		CorpusPairResultRef pair = m_model->pairAt(
		    index.row(), index.column(), &inverted);
		if (!pair)
		{
			m_details->setText("This binary pair is not part of the selected comparison plan.");
			return;
		}
		CorpusPairMetrics metrics = inverted ? invertCorpusMetrics(pair->metrics)
		                                      : pair->metrics;
		CorpusPairMetrics raw = inverted ? invertCorpusMetrics(pair->rawMetrics)
		                                  : pair->rawMetrics;
		const auto& source = m_binaries[static_cast<size_t>(index.row())];
		const auto& target = m_binaries[static_cast<size_t>(index.column())];
		QString state = pair->state == CorpusPairState::Cached ? "cached"
		    : pair->state == CorpusPairState::Complete ? "complete"
		    : pair->state == CorpusPairState::Failed ? "failed"
		    : pair->state == CorpusPairState::Cancelled ? "cancelled" : "pending";
		m_details->setText(QString(
		    "%1 -> %2. %3. Filtered instruction coverage %4%, function coverage %5%, %6 accepted matches. Raw instruction coverage %7%, function coverage %8%. Mean similarity %9%, confidence %10%.%11")
		    .arg(QString::fromStdString(source.displayName))
		    .arg(QString::fromStdString(target.displayName)).arg(state)
		    .arg(metrics.instructionCoverageSource * 100.0, 0, 'f', 1)
		    .arg(metrics.sourceCoverage * 100.0, 0, 'f', 1)
		    .arg(metrics.acceptedMatchCount)
		    .arg(raw.instructionCoverageSource * 100.0, 0, 'f', 1)
		    .arg(raw.sourceCoverage * 100.0, 0, 'f', 1)
		    .arg(metrics.meanSimilarity * 100.0, 0, 'f', 1)
		    .arg(metrics.meanConfidence * 100.0, 0, 'f', 1)
		    .arg(pair->error.empty() ? QString()
		        : QString(" Error: %1").arg(QString::fromStdString(pair->error))));
	}

	void openReuseRow(int row)
	{
		auto edge = m_reuseModel->edgeAt(row);
		if (!edge)
			return;
		auto source = currentlyOpen(edge->source);
		auto target = currentlyOpen(edge->target);
		if (!source || !target)
		{
			m_details->setText("Reopen both source tabs before opening this reuse pair.");
			return;
		}
		auto prepared = preparedCorpusPair(edge->source, edge->target);
		if (!prepared)
		{
			m_details->setText(
			    "This reuse pair is no longer the corpus snapshot. Open a new corpus workspace.");
			return;
		}
		openPairWorkspace(UIContext::activeContext(), *source, *target,
		    std::move(prepared), true);
	}

	std::vector<DiffWorkspacePane> m_panes;
	CorpusScheduler* m_scheduler = nullptr;
	uint64_t m_identityGeneration = 0;
	Ref<BackgroundTask> m_exportTask;
	QTimer* m_familyRebuildTimer = nullptr;
	QFutureWatcher<std::shared_ptr<FamilyBuildResult>>* m_familyWatcher = nullptr;
	uint64_t m_familyGeneration = 0;
	uint64_t m_familyWorkerGeneration = 0;
	uint64_t m_familyAppliedGeneration = 0;
	bool m_familyWorkerRunning = false;
	CorpusMatrixModel* m_model = nullptr;
	CorpusFamilyModel* m_familyModel = nullptr;
	CorpusChangesModel* m_changesModel = nullptr;
	CorpusChangesProxyModel* m_changesProxy = nullptr;
	QTableView* m_changesTable = nullptr;
	QPlainTextEdit* m_changeEvidence = nullptr;
	QPushButton* m_openTransition = nullptr;
	QPushButton* m_classifyTransition = nullptr;
	QPersistentModelIndex m_selectedTransition;
	CorpusLineageModel* m_lineageModel = nullptr;
	CorpusLineageProxyModel* m_lineageProxy = nullptr;
	QTableView* m_lineageTable = nullptr;
	CorpusReuseModel* m_reuseModel = nullptr;
	QTableView* m_reuseTable = nullptr;
	CorpusFamilyProxyModel* m_familyProxy = nullptr;
	QTableView* m_familyTable = nullptr;
	QLabel* m_familyCount = nullptr;
	QPlainTextEdit* m_patchEvidence = nullptr;
	std::vector<CorpusBinaryId> m_order;
	CorpusMode m_mode = CorpusMode::Reference;
	std::optional<CorpusBinaryId> m_reference;
	std::map<CorpusEdgeKey, CorpusPairResultRef> m_pairResults;
	std::map<CorpusEdgeKey, CorpusPairResultRef> m_patchPairResults;
	bool m_patchPairRun = false;
	CorpusMetricFilters m_metricFilters;
	FamilyBuildSettings m_familySettings;
	std::vector<CorpusBinary> m_binaries;
	CorpusPlan m_plan;
	std::optional<std::string> m_manifestPath;
	bool m_restoredManifest = false;
	bool m_discardedStalePatchSignature = false;
	std::optional<PatchSignature> m_patchSignature;
	std::map<CorpusBinaryId, PatchClassification> m_patchClassifications;
	double m_patchMinimumMargin = 0.1;
	double m_patchMinimumCoverage = 0.6;
	uint64_t m_lastRunElapsedMicroseconds = 0;
	uint64_t m_lastRunStartingResidentBytes = 0;
	uint64_t m_lastRunPeakResidentBytes = 0;
	QElapsedTimer m_workspaceRunTimer;
	QTableView* m_matrix = nullptr;
	QLabel* m_progressText = nullptr;
	QProgressBar* m_progress = nullptr;
	QLabel* m_details = nullptr;
	QPushButton* m_cancel = nullptr;
};

}  // namespace

void persistCorpusMatchesForFile(FileMetadata* file)
{
	std::vector<CorpusWorkspace*> workspaces(
	    g_corpusWorkspaces.begin(), g_corpusWorkspaces.end());
	for (CorpusWorkspace* workspace : workspaces)
		if (workspace)
			workspace->persistMatchesForFile(file);
}

void openCorpusWorkspace()
{
	UIContext* context = UIContext::activeContext();
	if (!context)
		return;
	std::vector<DiffWorkspacePane> openPanes = collectOpenDiffPanes(context);
	if (openPanes.size() < 3)
	{
		QMessageBox::warning(nullptr, "Binja Diff corpus",
		    "Open at least three different analyzed binaries first, then run this action again.");
		return;
	}

	CorpusSetupDialog dialog(openPanes);
	if (dialog.exec() != QDialog::Accepted)
		return;
	std::vector<size_t> selected = dialog.selectedIndices();
	auto reference = dialog.referenceIndex();
	if (selected.size() < 3 || !reference)
		return;

	std::vector<DiffWorkspacePane> panes;
	panes.reserve(selected.size());
	std::optional<size_t> referenceIndex;
	for (size_t sourceIndex : selected)
	{
		if (sourceIndex == *reference)
			referenceIndex = panes.size();
		panes.push_back(openPanes[sourceIndex]);
	}
	if (dialog.mode() != CorpusMode::Reference)
		referenceIndex = std::nullopt;
	if (dialog.mode() == CorpusMode::AllPairs)
	{
		size_t pairs = panes.size() * (panes.size() - 1) / 2;
		if (QMessageBox::question(nullptr, "Start all-pairs corpus",
		    QString("Run %1 pair comparisons now?").arg(pairs),
		    QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
			return;
	}
	// The workspace derives each binary's persistent identity, which hashes every
	// saved database on first use. Warm those hashes off the GUI thread.
	std::vector<BinaryViewRef> views;
	for (const auto& pane : panes)
		views.push_back(pane.data);
	if (!runPaneWorkWithProgress(context, panes, "Checking saved databases…", [views]() {
		    for (const auto& view : views)
			    DiffEngine::instance().persistentIdentity(view);
	    }))
		return;
	auto* workspace = new CorpusWorkspace(
	    std::move(panes), dialog.mode(), referenceIndex,
	    dialog.referenceOverlay(), dialog.reuseCachedPairs());
	context->createTabForWidget("Diff corpus", workspace);
}
