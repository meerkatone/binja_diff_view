#include "symbolport.h"

#include <exception>
#include <map>
#include <utility>

using namespace BinaryNinja;

SymbolPortApplyResult applySymbolPortPlan(BinaryNinja::Ref<BinaryNinja::BinaryView> target,
	const SymbolPortPlan& plan, SymbolPortCancelCheck cancelled, SymbolPortProgress progress)
{
	SymbolPortApplyResult result;
	if (!target)
	{
		result.error = "missing target binary view";
		return result;
	}

	try
	{
		std::map<uint64_t, Ref<Function>> targets;
		for (const auto& function : target->GetAnalysisFunctionList())
			if (function)
				targets.emplace(function->GetStart(), function);

		bool committed = target->RunUndoableTransaction([&]() {
#if BN_CURRENT_CORE_ABI_VERSION >= 180
			BulkSymbolModification bulk(target);
#else
			struct BulkSymbolModificationGuard
			{
				Ref<BinaryView> view;
				explicit BulkSymbolModificationGuard(Ref<BinaryView> targetView) :
				    view(std::move(targetView))
				{
					view->BeginBulkModifySymbols();
				}
				~BulkSymbolModificationGuard() { view->EndBulkModifySymbols(); }
			};
			BulkSymbolModificationGuard bulk(target);
#endif
			for (size_t index = 0; index < plan.renames.size(); index++)
			{
				if (cancelled && cancelled())
				{
					result.cancelled = true;
					break;
				}
				const auto& rename = plan.renames[index];
				auto function = targets.find(rename.targetAddress);
				if (function == targets.end() || !function->second)
				{
					result.changedAfterPreview++;
					continue;
				}
				Ref<Symbol> current = function->second->GetSymbol();
				std::string currentName = current ? current->GetShortName() : std::string();
				if (currentName != rename.oldName)
				{
					result.changedAfterPreview++;
					continue;
				}
				Ref<Symbol> symbol = new Symbol(FunctionSymbol, rename.newShortName,
				    rename.newFullName, rename.newRawName, rename.targetAddress);
				target->DefineUserSymbol(symbol);
				result.applied++;
				if (progress)
					progress(index + 1, plan.renames.size());
			}
			return true;
		});
		result.success = committed;
	}
	catch (const std::exception& exception)
	{
		result.error = exception.what();
	}
	catch (...)
	{
		result.error = "unknown symbol-port exception";
	}
	return result;
}
