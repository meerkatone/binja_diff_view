#pragma once

#include <set>
#include <memory>

namespace bn_graph_diff { struct FunctionMatchMaps; }
#include <cstdint>
#include <optional>
#include <utility>

#include <QtCore/QModelIndex>
#include <QtWidgets/QWidget>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QAbstractItemModel;
class QSortFilterProxyModel;
class QTableView;
class QTimer;
class ViewFrame;
class QPoint;
enum class SymbolPortDirection;

// Searchable/sortable whole-binary match navigator. The Rust engine remains
// the sole owner of match decisions; this widget is a native projection of its
// full result and drives the paired graph panes through DiffController.
class DiffResultsPanel : public QWidget
{
  public:
	explicit DiffResultsPanel(ViewFrame* left, ViewFrame* right, QWidget* parent = nullptr);

	void refresh();
	void beginInitialMatch();
	void showInitialError(const QString& error);
	void setLinearMode(bool linear);
	std::optional<std::pair<uint64_t, uint64_t>> selectedMatchAddresses() const;
	bool restoreSelectedGraphDiff();

  private:
	void rebuildFilterChoices();
	void applyFilter();
	void applyInitialColumnWidths();
	void updateSelectionDetails();
	void openSelected();
	void showContextMenu(const QPoint& position);
	void portSymbols(SymbolPortDirection direction, bool allVisible, bool currentPair = false,
	    std::optional<std::pair<uint64_t, uint64_t>> addressPair = std::nullopt);
	std::set<uint64_t> matchedAddresses(bool allVisible) const;
	QModelIndex selectedCurrentIndex() const;
	std::optional<std::pair<uint64_t, uint64_t>> selectedUnmatchedPair() const;

	std::shared_ptr<const bn_graph_diff::FunctionMatchMaps> m_resultSnapshot;
	ViewFrame* m_left = nullptr;
	ViewFrame* m_right = nullptr;
	QLabel* m_summary = nullptr;
	QLineEdit* m_search = nullptr;
	QComboBox* m_filter = nullptr;
	QCheckBox* m_changedOnly = nullptr;
	QCheckBox* m_ambiguousOnly = nullptr;
	bool m_hasRows = false;
	QTableView* m_tree = nullptr;
	QAbstractItemModel* m_model = nullptr;
	QSortFilterProxyModel* m_proxy = nullptr;
	QLabel* m_details = nullptr;
	QLabel* m_status = nullptr;
	QPushButton* m_open = nullptr;
	QTimer* m_filterTimer = nullptr;
	int m_startAttempts = 0;
	bool m_initialColumnWidthsApplied = false;
	bool m_linearMode = false;
};
