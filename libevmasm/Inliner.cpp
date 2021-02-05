/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * @file Inliner.cpp
 * Inlines small code snippets by replacing JUMP with a copy of the code jumped to.
 */

#include <libevmasm/Inliner.h>

#include <libevmasm/AssemblyItem.h>
#include <libevmasm/GasMeter.h>
#include <libevmasm/KnownState.h>
#include <libevmasm/SemanticInformation.h>

#include <libsolutil/CommonData.h>

#include <range/v3/numeric/accumulate.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/slice.hpp>
#include <range/v3/view/transform.hpp>

#include <optional>


using namespace std;
using namespace solidity;
using namespace solidity::evmasm;

bool Inliner::isInlineCandidate(u256 const& _tag, ranges::span<AssemblyItem const> _items) const
{
	assertThrow(_items.size() > 0, OptimizerException, "");

	// Only consider blocks that end in a JUMP for now. This can e.g. be extended to include transaction terminating
	// instructions as well in the future.
	if (_items.back() != Instruction::JUMP)
		return false;

	// Never inline tags that reference themselves.
	for (AssemblyItem const& item: _items)
		if (item == AssemblyItem{PushTag, _tag})
				return false;

	return true;
}

map<u256, Inliner::InlinableBlock> Inliner::determineInlinableBlocks(AssemblyItems const& _items) const
{
	std::map<u256, ranges::span<AssemblyItem const>> inlinableBlockItems;
	std::map<u256, uint64_t> numPushTags;
	std::optional<size_t> lastTag;
	for (auto&& [index, item]: _items | ranges::views::enumerate)
	{
		// The number of PushTags approximates the number of calls to a block.
		if (item.type() == PushTag)
			++numPushTags[item.data()];

		// We can only inline blocks with straight control flow that end in a jump.
		// Using breaksCSEAnalysisBlock will hopefully allow the return jump to be optimized after inlining.
		if (lastTag && SemanticInformation::breaksCSEAnalysisBlock(item, false))
		{
			ranges::span<AssemblyItem const> block = _items | ranges::views::slice(*lastTag + 1, index + 1);
			u256 tag = _items[*lastTag].data();
			if (isInlineCandidate(tag, block))
				inlinableBlockItems[tag] = block;
			lastTag.reset();
		}

		if (item.type() == Tag)
			lastTag = index;
	}

	// Store the number of PushTags alongside the assembly items and discard tags that are never pushed.
	map<u256, InlinableBlock> result;
	for (auto&& [tag, items]: inlinableBlockItems)
		if (uint64_t const* numPushes = util::valueOrNullptr(numPushTags, tag))
			result.emplace(tag, InlinableBlock{items, *numPushes});
	return result;
}

namespace
{
/// @returns an estimation of the runtime gas cost of the AsssemblyItems in @a _itemRange.
template<typename RangeType>
u256 executionCost(RangeType&& _itemRange, langutil::EVMVersion _evmVersion)
{
	GasMeter gasMeter{std::make_shared<KnownState>(), _evmVersion};
	auto gasConsumption = ranges::accumulate(std::forward<RangeType>(_itemRange) | ranges::views::transform(
		[&gasMeter](auto const& _item) { return gasMeter.estimateMax(_item, false); }
	), GasMeter::GasConsumption());
	if (gasConsumption.isInfinite)
		return numeric_limits<u256>::max();
	else
		return gasConsumption.value;
}
/// @returns an estimation of the code size in bytes needed for the AssemblyItems in @a _itemRange.
template<typename RangeType>
uint64_t codeSize(RangeType&& _itemRange)
{
	return ranges::accumulate(std::forward<RangeType>(_itemRange) | ranges::views::transform(
			[](auto const& _item) { return _item.bytesRequired(2); }
	), 0u);
}
}

bool Inliner::shouldInlineFullFunctionBody(ranges::span<AssemblyItem const> _block, uint64_t _pushTagCount) const
{
	// Accumulate size of the inline candidate block in bytes (without the return jump).
	uint64_t functionBodySize = codeSize(ranges::views::drop_last(_block, 1));

	// Use the number of push tags as approximation of the average number of calls to the function per run.
	uint64_t numberOfCalls = _pushTagCount;
	// Also use the number of push tags as approximation of the number of call sites to the function.
	uint64_t numberOfCallSites = _pushTagCount;

	static AssemblyItems const uninlinedCallSitePattern = {
		AssemblyItem{PushTag},
		AssemblyItem{PushTag},
		AssemblyItem{Instruction::JUMP},
		AssemblyItem{Tag}
	};
	static AssemblyItems const uninlinedFunctionPattern = {
		AssemblyItem{Tag},
		// Actual function body of size functionBodySize. Handled separately below.
		AssemblyItem{Instruction::JUMP}
	};

	// Both the call site and jump site pattern is executed for each call.
	// Since the function body has to be executed equally often both with and without inlining,
	// it can be ignored.
	bigint uninlinedExecutionCost = numberOfCalls * (
		executionCost(uninlinedCallSitePattern, m_evmVersion) +
		executionCost(uninlinedFunctionPattern, m_evmVersion)
	);
	// Each call site deposits the call site pattern, whereas the jump site pattern and the function itself are deposited once.
	bigint uninlinedDepositCost = GasMeter::dataGas(
		numberOfCallSites * codeSize(uninlinedCallSitePattern) +
		codeSize(uninlinedFunctionPattern) +
		functionBodySize,
		m_isCreation,
		m_evmVersion
	);
	// When inlining the execution cost beyond the actual function execution is zero,
	// but for each call site a copy of the function is deposited.
	bigint inlinedDepositCost = GasMeter::dataGas(numberOfCallSites * functionBodySize, m_isCreation, m_evmVersion);

	// If the estimated runtime cost over the lifetime of the contract plus the deposit cost in the uninlined case
	// exceed the inlined deposit costs, it is beneficial to inline.
	if (bigint(m_runs) * uninlinedExecutionCost + uninlinedDepositCost > inlinedDepositCost)
		return true;

	return false;
}


optional<AssemblyItem::JumpType> Inliner::shouldInline(AssemblyItem const& _jump, InlinableBlock const& _block) const
{
	AssemblyItem exitJump = _block.items.back();
	assertThrow(_jump == Instruction::JUMP && exitJump == Instruction::JUMP, OptimizerException, "");

	if (
		_jump.getJumpType() == AssemblyItem::JumpType::IntoFunction &&
		exitJump.getJumpType() == AssemblyItem::JumpType::OutOfFunction
	)
		return
			shouldInlineFullFunctionBody(_block.items, _block.pushTagCount) ?
			make_optional(AssemblyItem::JumpType::Ordinary) : nullopt;

	return nullopt;
}


void Inliner::optimise()
{
	std::map<u256, InlinableBlock> inlinableBlocks = determineInlinableBlocks(m_items);

	if (inlinableBlocks.empty())
		return;

	AssemblyItems newItems;
	for (auto it = m_items.begin(); it != m_items.end(); ++it)
	{
		AssemblyItem const& item = *it;
		if (next(it) != m_items.end())
		{
			AssemblyItem const& nextItem = *next(it);
			if (item.type() == PushTag && nextItem == Instruction::JUMP)
				if (auto* inlinableBlock = util::valueOrNullptr(inlinableBlocks, item.data()))
					if (auto exitJumpType = shouldInline(nextItem, *inlinableBlock))
					{
						newItems += inlinableBlock->items;
						newItems.back().setJumpType(*exitJumpType);

						// We are removing one push tag to the block we inline.
						--inlinableBlock->pushTagCount;
						// We might increase the number of push tags to other blocks.
						for (AssemblyItem const& inlinedItem: inlinableBlock->items)
							if (inlinedItem.type() == PushTag)
								if (auto* block = util::valueOrNullptr(inlinableBlocks, inlinedItem.data()))
									++block->pushTagCount;

						// Skip the original jump to the inlined tag and continue.
						++it;
						continue;
					}
		}
		newItems.emplace_back(item);
	}

	m_items = move(newItems);
}
