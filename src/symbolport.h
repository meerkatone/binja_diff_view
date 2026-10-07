// Binary Ninja-backed application of a dependency-free SymbolPortPlan.
// Kept outside the UI controller so the exact transaction can be exercised by
// headless integration tests and reused by future non-Qt frontends.
#pragma once

#include <functional>
#include <string>

#include "binaryninjaapi.h"
#include "diffmodel.h"

struct SymbolPortApplyResult
{
	bool success = false;
	bool cancelled = false;
	size_t applied = 0;
	size_t changedAfterPreview = 0;
	std::string error;
};

using SymbolPortCancelCheck = std::function<bool()>;
using SymbolPortProgress = std::function<void(size_t completed, size_t total)>;

// Applies all still-current entries as one Binary Ninja undo step. A target
// whose name changed after preview is skipped rather than overwritten.
SymbolPortApplyResult applySymbolPortPlan(
	BinaryNinja::Ref<BinaryNinja::BinaryView> target,
	const SymbolPortPlan& plan,
	SymbolPortCancelCheck cancelled = {},
	SymbolPortProgress progress = {});
