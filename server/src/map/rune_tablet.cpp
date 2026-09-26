// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// Rune Tablet: single translation unit. Public entry points are declared in pc.hpp.
// Sections: rune_tablet_logic (pure rules), rune_tablet_wire (packet layouts),
// then the server implementation.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include <common/database.hpp>
#include <common/random.hpp>
#include <common/timer.hpp>
#include <common/socket.hpp>
#include <common/showmsg.hpp>

#include "achievement.hpp"
#include "battle.hpp"
#include "clif.hpp"
#include "pc.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "script.hpp"
#include "status.hpp"

// ---- rune_tablet_logic: pure rules, no RNG or inventory access ----
namespace rune_tablet_logic {
// Legacy progress 1..16 remains valid. Upgraded state uses a version marker
// and packs level+1 plus a uint16 failure count in one character-registry value.
constexpr int64_t upgrade_state_marker = int64_t(1) << 32;
constexpr int level(int64_t progress) {
    if (progress >= 1 && progress <= 16)
        return static_cast<int>(progress - 1);
    if (progress < upgrade_state_marker)
        return -1;
    const int64_t payload = progress - upgrade_state_marker;
    if (payload <= 0xffff10
        && (payload & 255) >= 1 && (payload & 255) <= 16)
        return static_cast<int>((payload & 255) - 1);
    return -1;
}
constexpr int failures(int64_t progress) {
    return level(progress) < 0 ? -1 : progress < upgrade_state_marker ? 0
        : static_cast<int>((progress - upgrade_state_marker) >> 8);
}
constexpr int64_t progress_value(int upgrade, int failure_count) {
    if (upgrade < 0 || upgrade > 15 || failure_count < 0 || failure_count > 65535)
        return 0;
    return failure_count == 0 ? upgrade + 1
        : upgrade_state_marker + (int64_t(failure_count) << 8) + upgrade + 1;
}

struct effect_rule {
    uint16_t fragments = 0;
    uint16_t min_level = 0;
    uint16_t level_step = 1;
    int32_t base = 0;
    int32_t per_step = 0;
};

constexpr int32_t effect_value(const effect_rule& rule, uint16_t fragments, int upgrade) {
    if (upgrade < 0 || upgrade > 15 || !rule.level_step || fragments < rule.fragments || upgrade < rule.min_level)
        return 0;
    return rule.base + (upgrade / rule.level_step) * rule.per_step;
}

// Client 0x766f80: base + failures * increment; 100000 represents 100%.
// Keep raw arithmetic separate from probability saturation. This deliberately
// does not decide whether a successful upgrade resets the failure counter.
constexpr uint32_t upgrade_probability_scale = 100000;
constexpr uint64_t upgrade_rate_raw(uint32_t base, uint32_t increment, uint16_t failures) {
    return uint64_t(base) + uint64_t(increment) * failures;
}
constexpr uint32_t upgrade_chance(uint32_t base, uint32_t increment, uint16_t failures) {
    const auto raw = upgrade_rate_raw(base, increment, failures);
    return raw >= upgrade_probability_scale ? upgrade_probability_scale : static_cast<uint32_t>(raw);
}

// No RNG or inventory mutation here. A roll is uniform in [0,100000).
// Return zero for an invalid/non-upgradeable state; caller must not charge it.
constexpr int64_t upgrade_outcome(int64_t progress, uint32_t base, uint32_t increment, uint32_t roll) {
    const int current = level(progress), failed = failures(progress);
    if (current < 0 || current >= 15 || failed < 0 || roll >= upgrade_probability_scale
        || base > upgrade_probability_scale || increment > upgrade_probability_scale)
        return 0;
    if (roll < upgrade_chance(base, increment, static_cast<uint16_t>(failed)))
        return progress_value(current + 1, 0);
    return failed == 65535 ? 0 : progress_value(current, failed + 1);
}

// Collection rewards: seven slots per set, claimed strictly in order.
// Result codes are the client's 0x0c17 messages (msgstringtable 4202,
// 4210..4215): MSI_RUNESYSTEM_REWARD_FAIL, _CANNOT_FIND_ITEM,
// _MAX_INVENTORY, _HEAVY_INVENTORY, _OVER_ITEM_COUNT, _RUNE_UI_NOT_OPENED,
// _TABLE_NOT_EXIT.
enum reward_result : uint8_t {
    reward_ok = 0,
    reward_need_rune = 1,
    reward_mismatch = 2,
    reward_no_space = 3,
    reward_overweight = 4,
    reward_over_count = 5,
    reward_ui_closed = 6,
    reward_no_table = 7,
};
constexpr int reward_slot_count = 7;

// Next non-empty slot after last_claimed (0..7), or 0 when none remain.
constexpr int next_reward_slot(const uint32_t (&items)[reward_slot_count], int last_claimed) {
    if (last_claimed < 0 || last_claimed > reward_slot_count)
        return 0;
    for (int slot = last_claimed + 1; slot <= reward_slot_count; ++slot)
        if (items[slot - 1])
            return slot;
    return 0;
}

// Client 0x81cfd0/0x766c70/0x767fa0: slot 1 needs the set activated, slots
// 2..6 at least that many active fragments, slot 7 only the preceding claim.
constexpr uint8_t reward_check(const uint32_t (&items)[reward_slot_count], int last_claimed,
        int requested, bool set_active, int active_fragments) {
    if (!set_active)
        return reward_need_rune;
    const int next = next_reward_slot(items, last_claimed);
    if (!next || requested != next)
        return reward_mismatch;
    if (requested >= 2 && requested <= 6 && active_fragments < requested)
        return reward_need_rune;
    return reward_ok;
}

struct material { uint32_t id; uint32_t amount; };
struct slot { uint32_t id; uint32_t amount; bool eligible; };
struct debit { std::size_t index; uint32_t amount; };

// Build the complete debit plan before modifying inventory. A missing material
// never returns a partial plan; split stacks and duplicate costs are supported.
inline std::optional<std::vector<debit>> plan(
	const std::vector<material>& costs, const std::vector<slot>& inventory) {
	if (costs.empty())
		return std::nullopt;
	std::map<uint32_t, uint64_t> remaining;
	for (const auto& cost : costs) {
		if (!cost.id || !cost.amount)
			return std::nullopt;
		remaining[cost.id] += cost.amount;
	}
	std::vector<debit> result;
	for (std::size_t i = 0; i < inventory.size(); ++i) {
		const auto& item = inventory[i];
		auto required = remaining.find(item.id);
		if (!item.eligible || required == remaining.end() || !required->second)
			continue;
		uint32_t amount = static_cast<uint32_t>(required->second < item.amount ? required->second : item.amount);
		if (amount) {
			result.push_back({i, amount});
			required->second -= amount;
		}
	}
	for (const auto& cost : remaining)
		if (cost.second)
			return std::nullopt;
	return result;
}
}

// ---- rune_tablet_wire: packet layouts ----
// Names are descriptive, not recovered Gravity symbols. See doc/rune_tablet.md.
namespace rune_tablet_wire {
enum : uint16_t {
	request_category = 0x0bcb,
	fragment_list = 0x0bcc,
	set_list = 0x0bcd,
	activate_fragment = 0x0bce,
	fragment_result = 0x0bcf,
	activate_set = 0x0bd0,
	set_result = 0x0bd1,
	upgrade_set = 0x0bd2,
	upgrade_result = 0x0bd3,
	unequip = 0x0bd4,
	unequip_result = 0x0bd5,
	equip = 0x0bd6,
	equip_result = 0x0bd7,
	decompose = 0x0bd8,
	decompose_result = 0x0bd9,
	open_window = 0x0bdf,
	window_state = 0x0be0, // CZ: byte 2 is 1 from the window constructor, 0 from its destructor
	equipped_state = 0x0bf2,
	reward_list = 0x0c15,
	claim_reward = 0x0c16,
	reward_result = 0x0c17,
};

constexpr void put16(uint8_t* p, uint16_t value) {
	p[0] = static_cast<uint8_t>(value);
	p[1] = static_cast<uint8_t>(value >> 8);
}

constexpr void put32(uint8_t* p, uint32_t value) {
	put16(p, static_cast<uint16_t>(value));
	put16(p + 2, static_cast<uint16_t>(value >> 16));
}

constexpr uint16_t get16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
}

constexpr uint32_t get32(const uint8_t* p) {
	return get16(p) | (uint32_t(get16(p + 2)) << 16);
}

constexpr std::array<uint8_t, 3> window(bool open) {
	std::array<uint8_t, 3> p{};
	put16(p.data(), open_window);
	p[2] = open ? 1 : 0;
	return p;
}

// Empty character state: length, reserved byte, category, entry count.
constexpr std::array<uint8_t, 9> empty_list(uint16_t type, uint16_t category) {
	std::array<uint8_t, 9> p{};
	put16(p.data(), type);
	put16(p.data() + 2, static_cast<uint16_t>(p.size()));
	put16(p.data() + 5, category);
	return p;
}

// Result 11 maps to MSI_RUNESYSTEM_DB_LOAD_NOT_YET in the inspected client.
template<std::size_t N>
constexpr std::array<uint8_t, N> operation(uint16_t type, uint8_t result, uint16_t category, uint32_t id) {
	static_assert(N == 9 || N == 13, "Rune operation response size");
	std::array<uint8_t, N> p{};
	put16(p.data(), type);
	p[2] = result;
	put16(p.data() + 3, category);
	put32(p.data() + 5, id);
	return p;
}

// 0x0bd3 uses the same dispatcher as activation (0xd14874). Result 0
// updates state for BOTH success and a failed roll: the client distinguishes
// them by the changed level/failure count (0x768960), not a separate error.
constexpr std::array<uint8_t, 13> upgrade_state(uint8_t result, uint16_t category,
        uint32_t id, uint16_t level, uint16_t failures) {
    auto packet = operation<13>(upgrade_result, result, category, id);
    put16(packet.data() + 9, level);
    put16(packet.data() + 11, failures);
    return packet;
}

// 0x0bd8: item ID, client inventory index (server index + 2), mode 1/2.
// Same item-object fields as CZ_ITEM_REFORM, with a wider index on this wire.
// Validate the full uint32 index before subtraction/narrowing or inventory access.
struct decomposition_request { uint32_t item_id; uint32_t server_index; uint32_t mode; };
constexpr bool read_decomposition_request(const uint8_t* data, std::size_t length,
        uint32_t inventory_capacity, decomposition_request& request) {
    if (!data || length != 14 || get16(data) != decompose)
        return false;
    const uint32_t item_id = get32(data + 2);
    const uint32_t index = get32(data + 6);
    const uint32_t mode = get32(data + 10);
    if (!item_id || index < 2 || index - 2 >= inventory_capacity || (mode != 1 && mode != 2))
        return false;
    request = {item_id, index - 2, mode};
    return true;
}

// 0x0bd9: eight uint32 IDs, then eight int16 quantities, then result.
// Confirmed by dialog 0x16a handler 0x81e3f0 and receiver 0xd66470.
// Quantity sign-extension in the client forbids values above 32767.
// Serializes processed results; inventory delivery is handled by the caller.
struct decomposition_output { uint32_t id; uint32_t amount; };
constexpr bool decomposition_success(const std::array<decomposition_output, 8>& outputs,
        std::array<uint8_t, 51>& packet) {
    std::array<uint8_t, 51> next{};
    put16(next.data(), decompose_result);
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto& entry = outputs[i];
        if ((entry.id == 0) != (entry.amount == 0) || entry.amount > 32767)
            return false;
        put32(next.data() + 2 + i * 4, entry.id);
        put16(next.data() + 34 + i * 2, static_cast<uint16_t>(entry.amount));
    }
    // Result zero at byte 50 is a processed success, including empty output.
    packet = next;
    return true;
}

// 0x0c15 entries: uint32 set ID + uint8 last claimed slot (not a bitmask).
// The list keeps the same 9-byte header as empty_list; receiver 0xd672e0.
constexpr std::array<uint8_t, 5> reward_entry(uint32_t id, uint8_t last_claimed_slot) {
    std::array<uint8_t, 5> packet{};
    put32(packet.data(), id);
    packet[4] = last_claimed_slot;
    return packet;
}

// 0x0c17 dispatcher 0xd1493f, handler 0xd671a0, manager 0x768a00.
// Result 0 applies last_claimed_slot, a sequential value in 0..7.
// Prepared serializer only: reward delivery is not enabled by this helper.
constexpr std::array<uint8_t, 10> reward_state(uint8_t result, uint16_t category,
        uint32_t id, uint8_t last_claimed_slot) {
    std::array<uint8_t, 10> packet{};
    put16(packet.data(), reward_result);
    packet[2] = result;
    put16(packet.data() + 3, category);
    put32(packet.data() + 5, id);
    packet[9] = last_claimed_slot;
    return packet;
}

// 0x0bf2: result, category, set ID, upgrade level, active fragment count.
// Inspected dispatcher 0xd148b6 and manager 0x7687b0 (not failure count).
constexpr std::array<uint8_t, 13> equipped(uint16_t category, uint32_t id, uint16_t level, uint16_t fragments) {
    auto packet = operation<13>(equipped_state, 0, category, id);
    put16(packet.data() + 9, level);
    put16(packet.data() + 11, fragments);
    return packet;
}

template<std::size_t N>
constexpr std::array<uint8_t, N> unavailable(uint16_t type, uint16_t category, uint32_t id) {
	return operation<N>(type, 11, category, id);
}
}

// ---- server implementation ----
namespace {
bool valid_bonus_shape(int32 bonus, bool parameter) {
	switch (bonus) {
	case SP_ALL_STATS:
	case SP_ASPD_RATE:
	case SP_ATK_RATE:
	case SP_BASE_ATK:
	case SP_CON:
	case SP_CRATE:
	case SP_CRIT_ATK_RATE:
	case SP_CRITICAL:
	case SP_CRT:
	case SP_DEF1:
	case SP_DELAYRATE:
	case SP_HIT:
	case SP_NON_CRIT_ATK_RATE:
	case SP_LONG_ATK_RATE:
	case SP_EMATK:
	case SP_MATK_RATE:
	case SP_MAXHP:
	case SP_MAXHPRATE:
	case SP_MAXSP:
	case SP_MAXSPRATE:
	case SP_MDEF1:
	case SP_MRES:
	case SP_PATK:
	case SP_POW:
	case SP_RES:
	case SP_SHORT_ATK_RATE:
	case SP_SMATK:
	case SP_SPL:
	case SP_STA:
	case SP_SPRATE:
	case SP_VARCASTRATE:
	case SP_WIS:
		return !parameter;
	case SP_ADDCLASS:
	case SP_ADDELE:
	case SP_ADDRACE:
	case SP_ADDRACE2:
	case SP_ADDSIZE:
	case SP_EXP_ADDRACE:
	case SP_HP_DRAIN_RATE:
	case SP_MAGIC_ADDCLASS:
	case SP_MAGIC_ADDELE:
	case SP_MAGIC_ADDRACE:
	case SP_MAGIC_ADDRACE2:
	case SP_MAGIC_ADDSIZE:
	case SP_MAGIC_ATK_ELE:
	case SP_SP_DRAIN_RATE:
	case SP_MAGIC_SUBDEF_ELE:
	case SP_SUBDEF_ELE:
	case SP_SUBELE:
		return parameter;
	default:
		return false;
	}
}

// Special effects bypass pc_bonus entirely: they need filtering (e.g. targeted
// magic only, no ground/self skills) that the generic stat-bonus pool cannot
// express. Applied by dedicated hooks such as rune_tablet_on_targeted_magic_kill.
enum class rune_special : uint8 {
	none,
	targeted_magic_kill_recovery_hp,
	targeted_magic_kill_recovery_sp,
	// Fixed-rate proc, not level-scaled: Fragments/MinLevel gate whether it is
	// active at all. Rate/amount/ticks/interval are hardcoded in rune_tablet_on_hit
	// and SC_RUNE_TABLET_REGEN's timer case, matching the reviewed Korean source
	// (2% on hit, 200 SP every 1s, 4 times) rather than the client's own numbers.
	on_hit_sp_regen_chance,
};

struct rune_effect {
	rune_tablet_logic::effect_rule rule;
	int32 bonus = 0;
	int32 parameter = 0;
	bool has_parameter = false;
	rune_special special = rune_special::none;
};

struct rune_upgrade {
    uint32 chance = 0; // 100000 = 100%; client GradeTable.
    uint32 failure_increment = 0;
    std::vector<rune_tablet_logic::material> materials;
};

struct rune_record {
	uint32 id = 0;
	uint16 category = 0;
	bool is_set = false;
	std::vector<rune_tablet_logic::material> materials;
	std::vector<uint32> fragments;
	std::vector<rune_effect> effects;
	bool effect_free = false; // Officially grants nothing, yet may be equipped.
	uint32 rewards[rune_tablet_logic::reward_slot_count] = {}; // Collection reward per slot; 0 = empty.
	std::map<uint16, rune_upgrade> upgrades; // Target levels 1..15; no attempt policy implied.
};

class RuneTabletDatabase : public TypesafeYamlDatabase<uint32, rune_record> {
public:
	RuneTabletDatabase() : TypesafeYamlDatabase("RUNE_TABLET_DB", 1) {}
	const std::string getDefaultLocation() override { return "db/re/rune_tablet_db.yml"; }
	uint64 parseBodyNode(const ryml::NodeRef& node) override {
		auto record = std::make_shared<rune_record>();
		std::string type;
		if (!asUInt32(node, "Id", record->id) || !record->id
			|| !asUInt16(node, "Category", record->category) || !record->category
			|| !asString(node, "Type", type) || !nodesExist(node, {"Materials"}))
			return 0;
		if (type != "Fragment" && type != "Set") {
			invalidWarning(node, "Rune Tablet Type must be Fragment or Set.\n");
			return 0;
		}
		record->is_set = type == "Set";
		if (exists(record->id)) {
			invalidWarning(node, "Duplicate Rune Tablet Id %u.\n", record->id);
			return 0;
		}
		if (!node["Materials"].is_seq())
			return 0;
		for (const auto& material : node["Materials"]) {
			rune_tablet_logic::material cost{};
			if (!asUInt32(material, "Item", cost.id) || !cost.id
				|| !asUInt32(material, "Amount", cost.amount) || !cost.amount || cost.amount > MAX_AMOUNT)
				return 0;
			record->materials.push_back(cost);
		}
		if (record->materials.empty() || record->materials.size() > MAX_INVENTORY)
			return 0;
        if (nodeExists(node, "Upgrades")) {
            if (!record->is_set || !node["Upgrades"].is_seq()) {
                invalidWarning(node, "Rune Tablet Upgrades must be a sequence on a set.\n");
                return 0;
            }
            for (const auto& upgrade : node["Upgrades"]) {
                uint16 level;
                rune_upgrade entry;
                if (!asUInt16(upgrade, "Level", level) || level < 1 || level > 15
                    || record->upgrades.count(level)
                    || !asUInt32(upgrade, "Chance", entry.chance) || entry.chance > rune_tablet_logic::upgrade_probability_scale
                    || !asUInt32(upgrade, "FailureIncrement", entry.failure_increment) || entry.failure_increment > rune_tablet_logic::upgrade_probability_scale
                    || !nodeExists(upgrade, "Materials") || !upgrade["Materials"].is_seq()) {
                    invalidWarning(upgrade, "Invalid Rune Tablet upgrade level or rate.\n");
                    return 0;
                }
                std::map<uint32, uint32> amounts;
                for (const auto& material : upgrade["Materials"]) {
                    uint32 id, amount;
                    if (!asUInt32(material, "Item", id) || !id
                        || !asUInt32(material, "Amount", amount) || !amount || amount > MAX_AMOUNT
                        || amounts[id] > MAX_AMOUNT - amount) {
                        invalidWarning(material, "Invalid Rune Tablet upgrade material.\n");
                        return 0;
                    }
                    amounts[id] += amount;
                }
                if (amounts.empty() || amounts.size() > MAX_INVENTORY)
                    return 0;
                for (const auto& amount : amounts)
                    entry.materials.push_back({amount.first, amount.second});
                record->upgrades.emplace(level, std::move(entry));
            }
            if (record->upgrades.size() != 15) {
                invalidWarning(node, "Rune Tablet Upgrades must cover all 15 target levels.\n");
                return 0;
            }
        }
		if (nodeExists(node, "Fragments")) {
			if (!record->is_set || !node["Fragments"].is_seq())
				return 0;
			for (const auto& fragment : node["Fragments"]) {
				uint32 id;
				if (!asUInt32(fragment, "Id", id) || !id
					|| std::find(record->fragments.begin(), record->fragments.end(), id) != record->fragments.end())
					return 0;
				record->fragments.push_back(id);
			}
			if (record->fragments.size() < 2 || record->fragments.size() > 6)
				return 0;
		}
		if (nodeExists(node, "Effects")) {
			if (!record->is_set || record->fragments.empty() || !node["Effects"].is_seq())
				return 0;
			for (const auto& effect : node["Effects"]) {
				rune_effect entry;
				if (!asUInt16(effect, "Fragments", entry.rule.fragments)
					|| entry.rule.fragments < 2 || entry.rule.fragments > record->fragments.size()
					|| !asUInt16(effect, "MinLevel", entry.rule.min_level) || entry.rule.min_level > 15
					|| !asUInt16(effect, "LevelStep", entry.rule.level_step)
					|| entry.rule.level_step < 1 || entry.rule.level_step > 15
					|| !asInt32(effect, "Base", entry.rule.base)
					|| !asInt32(effect, "PerStep", entry.rule.per_step)
					|| entry.rule.base < -10000 || entry.rule.base > 10000
					|| entry.rule.per_step < -10000 || entry.rule.per_step > 10000) {
					invalidWarning(effect, "Invalid Rune Tablet effect.\n");
					return 0;
				}
				if (nodeExists(effect, "Special")) {
					std::string special;
					if (nodeExists(effect, "Bonus") || nodeExists(effect, "Parameter") || !asString(effect, "Special", special)) {
						invalidWarning(effect, "Invalid Rune Tablet special effect.\n");
						return 0;
					}
					if (special == "TargetedMagicKillRecoveryHP")
						entry.special = rune_special::targeted_magic_kill_recovery_hp;
					else if (special == "TargetedMagicKillRecoverySP")
						entry.special = rune_special::targeted_magic_kill_recovery_sp;
					else if (special == "OnHitSPRegenChance")
						entry.special = rune_special::on_hit_sp_regen_chance;
					else {
						invalidWarning(effect, "Unknown Rune Tablet special effect '%s'.\n", special.c_str());
						return 0;
					}
				} else {
					std::string bonus;
					int64 constant;
					if (!asString(effect, "Bonus", bonus) || bonus.empty() || bonus[0] != 'b'
						|| !script_get_constant(bonus.c_str(), &constant)
						|| constant < 0 || constant > std::numeric_limits<int32>::max()) {
						invalidWarning(effect, "Invalid Rune Tablet effect.\n");
						return 0;
					}
					entry.bonus = static_cast<int32>(constant);
					if (nodeExists(effect, "Parameter")) {
						std::string parameter;
						if (!asString(effect, "Parameter", parameter))
							return 0;
						if (!script_get_constant(parameter.c_str(), &constant)) {
							if (!asInt32(effect, "Parameter", entry.parameter))
								return 0;
						} else {
							if (constant < 0 || constant > std::numeric_limits<int32>::max())
								return 0;
							entry.parameter = static_cast<int32>(constant);
						}
						entry.has_parameter = true;
					}
					if (!valid_bonus_shape(entry.bonus, entry.has_parameter) || entry.parameter < 0) {
						invalidWarning(effect, "Unsupported Rune Tablet bonus or argument count.\n");
						return 0;
					}
				}
				record->effects.push_back(entry);
			}
			if (record->effects.empty() || record->effects.size() > 128)
				return 0;
		}
		if (nodeExists(node, "Rewards")) {
			if (!record->is_set || !node["Rewards"].is_seq()) {
				invalidWarning(node, "Rune Tablet Rewards must be a sequence on a set.\n");
				return 0;
			}
			for (const auto& reward : node["Rewards"]) {
				uint16 slot;
				uint32 item;
				if (!asUInt16(reward, "Slot", slot) || slot < 1 || slot > rune_tablet_logic::reward_slot_count
					|| record->rewards[slot - 1] || !asUInt32(reward, "Item", item) || !item) {
					invalidWarning(reward, "Invalid Rune Tablet reward slot.\n");
					return 0;
				}
				// Kept even if absent: skipping it would shift the client's claim sequence.
				if (!item_db.exists(item))
					invalidWarning(reward, "Rune Tablet reward item %u is absent from the item database; that slot cannot be claimed.\n", item);
				record->rewards[slot - 1] = item;
			}
			if (!record->rewards[0]) {
				invalidWarning(node, "Rune Tablet Rewards need the activation slot 1.\n");
				return 0;
			}
		}
		if (nodeExists(node, "EffectFree")) {
			if (!record->is_set || !record->effects.empty() || record->fragments.empty()
				|| !asBool(node, "EffectFree", record->effect_free)) {
				invalidWarning(node, "Rune Tablet EffectFree requires a set without Effects.\n");
				return 0;
			}
		}
		put(record->id, record);
		return 1;
	}
} rune_db;

struct decomposition_output {
	t_itemid item = 0;
	uint16 minimum = 0;
	uint16 maximum = 0;
	uint32 chance = 0; // 100000 = 100%; random draw relationships remain unverified.
};

struct decomposition_mode {
	uint16 consume = 0; // Cards taken from the selected stack (client 1/30).
	std::vector<decomposition_output> outputs;
};

struct decomposition_record {
	t_itemid item = 0;
	decomposition_mode modes[2]; // Request modes 1 and 2.
};

// Generated from the client's System/Rune/itemDecom.lub. Loading it only
// enables validation; the amount distribution is not verified, so nothing is
// consumed or delivered yet.
class RuneTabletDecompositionDatabase : public TypesafeYamlDatabase<t_itemid, decomposition_record> {
public:
	RuneTabletDecompositionDatabase() : TypesafeYamlDatabase("RUNE_TABLET_DECOMPOSITION_DB", 1) {}
	const std::string getDefaultLocation() override { return "db/re/rune_tablet_decomposition.yml"; }
	uint64 parseBodyNode(const ryml::NodeRef& node) override {
		auto record = std::make_shared<decomposition_record>();
		if (!asUInt32(node, "Item", record->item) || !record->item
			|| !nodesExist(node, {"Modes"}) || !node["Modes"].is_seq())
			return 0;
		if (exists(record->item)) {
			invalidWarning(node, "Duplicate Rune Tablet decomposition item %u.\n", record->item);
			return 0;
		}
		if (!item_db.exists(record->item)) {
			invalidWarning(node, "Rune Tablet decomposition item %u is absent from the item database, skipping.\n", record->item);
			return 0;
		}
		bool seen[2] = {};
		for (const auto& mode_node : node["Modes"]) {
			uint16 mode;
			if (!asUInt16(mode_node, "Mode", mode) || mode < 1 || mode > 2 || seen[mode - 1]) {
				invalidWarning(mode_node, "Rune Tablet decomposition Mode must be 1 or 2, once each.\n");
				return 0;
			}
			seen[mode - 1] = true;
			auto& entry = record->modes[mode - 1];
			if (!asUInt16(mode_node, "Consume", entry.consume) || !entry.consume || entry.consume > MAX_AMOUNT
				|| !nodeExists(mode_node, "Outputs") || !mode_node["Outputs"].is_seq()) {
				invalidWarning(mode_node, "Invalid Rune Tablet decomposition batch.\n");
				return 0;
			}
			for (const auto& output_node : mode_node["Outputs"]) {
				decomposition_output output;
				// Amounts travel as int16 in 0xbd9 and must fit a stack.
				if (!asUInt32(output_node, "Item", output.item) || !output.item
					|| !asUInt16(output_node, "Minimum", output.minimum) || !output.minimum
					|| !asUInt16(output_node, "Maximum", output.maximum) || output.maximum < output.minimum
					|| output.maximum > std::min<uint16>(MAX_AMOUNT, 32767)
					|| !asUInt32(output_node, "Chance", output.chance) || !output.chance
					|| output.chance > rune_tablet_logic::upgrade_probability_scale) {
					invalidWarning(output_node, "Invalid Rune Tablet decomposition output.\n");
					return 0;
				}
				if (!item_db.exists(output.item)) {
					invalidWarning(output_node, "Rune Tablet decomposition output %u is absent from the item database.\n", output.item);
					return 0;
				}
                if (std::any_of(entry.outputs.begin(), entry.outputs.end(),
                    [&](const decomposition_output& existing) { return existing.item == output.item; })) {
                    invalidWarning(output_node, "Duplicate Rune Tablet decomposition output item %u.\n", output.item);
                    return 0;
                }
				entry.outputs.push_back(output);
			}
			// 0xbd9 has exactly eight result positions.
			if (entry.outputs.empty() || entry.outputs.size() > 8) {
				invalidWarning(mode_node, "Rune Tablet decomposition needs one to eight outputs.\n");
				return 0;
			}
		}
		if (!seen[0] || !seen[1]) {
			invalidWarning(node, "Rune Tablet decomposition item %u needs both modes.\n", record->item);
			return 0;
		}
		put(record->item, record);
		return 1;
	}
} decomposition_db;

// Worst-case check before anything is consumed: every output at its maximum
// must fit the inventory and weight limit (user policy: reject, never drop).
const char* decomposition_capacity_error(map_session_data& sd, const decomposition_mode& mode) {
	uint64 weight = 0;
	int32 new_slots = 0;
	for (const auto& output : mode.outputs) {
		auto data = item_db.find(output.item);
		if (!data)
			return "Rune Tablet: a resulting item is missing from the item database.";
        // Match the plain, identified, unbound item that delivery will create.
        // pc_checkadditem only compares nameid and can reuse an incompatible
        // bound/rental/carded stack in its estimate; pc_additem does not.
        const bool stackable = itemdb_isstackable2(data.get());
        const uint32 limit = data->stack.inventory
            ? std::min<uint32>(MAX_AMOUNT, data->stack.amount) : MAX_AMOUNT;
        if (output.maximum > limit)
            return "Rune Tablet: you would exceed the amount limit of a resulting item.";
        bool existing_stack = false;
        if (stackable && !data->flag.guid) {
            for (int32 i = 0; i < MAX_INVENTORY; ++i) {
                const auto& current = sd.inventory.u.items_inventory[i];
                if (current.nameid != output.item || current.bound || current.expire_time || current.unique_id
                    || std::any_of(std::begin(current.card), std::end(current.card),
                        [](const auto card) { return card != 0; }))
                    continue;
                // pc_additem stops at the first compatible stack, including
                // when it is full or outside the usable inventory slots.
                if (i >= sd.status.inventory_slots || current.amount < 0
                    || uint64(current.amount) + output.maximum > limit)
                    return "Rune Tablet: you would exceed the amount limit of a resulting item.";
                existing_stack = true;
                break;
            }
        }
        if (!existing_stack)
            new_slots += stackable ? 1 : output.maximum;
		weight += uint64(data->weight) * output.maximum;
	}
	if (new_slots > pc_inventoryblank(&sd))
		return "Rune Tablet: not enough inventory space for the maximum result.";
	if (sd.weight + weight > uint64(sd.max_weight))
		return "Rune Tablet: the weight of the maximum result would exceed your limit.";
	return nullptr;
}

int64 progress_key(const rune_record& record) {
	return reference_uid(add_str(record.is_set ? "RuneTabletSet" : "RuneTabletFragment"), record.id);
}

bool owned(const map_session_data& sd, const rune_record& record) {
	return rune_tablet_logic::level(pc_readregistry(&sd, progress_key(record))) >= 0;
}

int64 equipped_key() { return add_str("RuneTabletEquipped"); }

// Last claimed collection reward slot (0..7) per set and character.
int64 reward_key(const rune_record& record) {
	return reference_uid(add_str("RuneTabletReward"), record.id);
}

bool has_rewards(const rune_record& record) {
	return rune_tablet_logic::next_reward_slot(record.rewards, 0) != 0;
}

bool valid_fragments(const rune_record& record) {
	if (record.fragments.size() < 2 || record.fragments.size() > 6)
		return false;
	for (uint32 id : record.fragments) {
		auto fragment = rune_db.find(id);
		if (!fragment || fragment->is_set || fragment->category != record.category)
			return false;
	}
	return true;
}

// Reviewed effects, or an official effect-free set; pending sets stay blocked.
bool equippable(const rune_record& record) {
	return (!record.effects.empty() || record.effect_free) && valid_fragments(record);
}

// Access gate; battle config rune_tablet_min_group_level (99 = GM only).
bool access_allowed(const map_session_data& sd) {
	return pc_get_group_level(&sd) >= battle_config.rune_tablet_min_group_level;
}

std::shared_ptr<rune_record> equipped_record(const map_session_data& sd) {
	if (!sd.vars_ok || !access_allowed(sd))
		return nullptr;
	const int64 id = pc_readregistry(&sd, equipped_key());
	if (id <= 0 || id > std::numeric_limits<uint32>::max())
		return nullptr;
	auto record = rune_db.find(static_cast<uint32>(id));
	if (!record || !record->is_set || !owned(sd, *record) || !equippable(*record))
		return nullptr;
	return record;
}

uint16 active_fragments(const map_session_data& sd, const rune_record& record) {
	uint16 count = 0;
	for (uint32 id : record.fragments) {
		auto fragment = rune_db.find(id);
		if (fragment && !fragment->is_set && fragment->category == record.category && owned(sd, *fragment))
			++count;
	}
	return count;
}

bool eligible(const item& item) {
	// Only ordinary, permanent inventory materials. Do not silently dismantle
	// equipped, favorite, rented, forged, carded, refined or enchanted items.
	if (item.amount <= 0 || item.equip || item.equipSwitch || item.favorite
		|| item.expire_time || item.refine || item.enchantgrade || item.attribute)
		return false;
	for (const auto card : item.card)
		if (card)
			return false;
	for (const auto& option : item.option)
		if (option.id || option.value || option.param)
			return false;
	return true;
}

// All delivery targets are plain stackable runes, distinct from the source.
// Keep this restricted until other item kinds and their hooks are reviewed.
int32 decomposition_target(map_session_data& sd, t_itemid id) {
    for (int32 i = 0; i < MAX_INVENTORY; ++i) {
        const auto& entry = sd.inventory.u.items_inventory[i];
        if (entry.nameid == id && !entry.bound && !entry.expire_time && !entry.unique_id
            && std::all_of(std::begin(entry.card), std::end(entry.card), [](const auto card) { return card == 0; }))
            return i;
    }
    return pc_search_inventory(&sd, 0);
}

const char* deliver_decomposition(map_session_data& sd,
        const rune_tablet_wire::decomposition_request& request, const decomposition_mode& mode,
        std::array<uint8_t, 51>& response) {
    const bool fixed = request.item_id == 1001595 && mode.outputs.size() == 1
        && mode.consume == (request.mode == 1 ? 1 : 30)
        && mode.outputs[0].item == 1001283 && mode.outputs[0].chance == 100000
        && mode.outputs[0].minimum == mode.consume && mode.outputs[0].maximum == mode.consume;
    if (!fixed && !battle_config.rune_tablet_decomposition_uniform)
        return "Rune Tablet: amount distribution is unverified; the local policy is disabled.";
    const item original = sd.inventory.u.items_inventory[request.server_index];
    if (original.bound || original.unique_id || request.server_index >= sd.status.inventory_slots)
        return "Rune Tablet: the item is bound, unique, or outside the available slots.";
    for (const auto& output : mode.outputs) {
        auto data = item_db.find(output.item);
        if ((output.item != 1001282 && output.item != 1001283) || output.item == original.nameid
            || !data || !itemdb_isstackable2(data.get()) || data->flag.guid || data->flag.autoequip)
            return "Rune Tablet: result configuration is incompatible with this conversion.";
    }
    // Optional LOCAL policy: independently roll each output's chance, then
    // sample its quantity uniformly in the inclusive interval. No rate changes.
    std::array<rune_tablet_wire::decomposition_output, 8> results{};
    for (size_t i = 0; i < mode.outputs.size(); ++i) {
        const auto& output = mode.outputs[i];
        if (output.chance == 100000 || rnd_value<uint32>(0, 99999) < output.chance) {
            const uint32 quantity = output.minimum == output.maximum ? output.minimum
                : rnd_value<uint32>(output.minimum, output.maximum);
            results[i] = {output.item, quantity};
        }
    }
    // Prepare the native response before charging anything.
    if (!rune_tablet_wire::decomposition_success(results, response))
        return "Rune Tablet: could not prepare the decomposition response.";
    struct granted_slot { int32 index; item before; item_data* data; uint32 amount; };
    std::vector<granted_slot> granted;
    granted.reserve(results.size()); // No vector allocation after source debit.
    auto* original_data = sd.inventory_data[request.server_index];
    const auto original_weight = sd.weight;
    const auto original_last_added = sd.last_addeditem_index;
    if (pc_delitem(&sd, request.server_index, mode.consume, 0, 0, LOG_TYPE_OTHER, false))
        return "Rune Tablet: could not consume the material.";
    for (const auto& result : results) {
        if (!result.amount)
            continue;
        const int32 index = decomposition_target(sd, result.id);
        bool failed = index < 0 || index >= sd.status.inventory_slots;
        granted_slot snapshot{};
        if (!failed) {
            snapshot = {index, sd.inventory.u.items_inventory[index], sd.inventory_data[index], result.amount};
            item next{};
            next.nameid = result.id;
            next.identify = 1;
            // For these plain, non-GUID, non-autoequip, permanent outputs,
            // pc_additem error returns precede inventory mutation.
            failed = pc_additem(&sd, &next, result.amount, LOG_TYPE_OTHER, false, false) != ADDITEM_SUCCESS;
        }
        if (!failed) {
            granted.push_back(snapshot);
            continue;
        }
        // Undo already delivered outputs in reverse order, including outputs
        // that reused the emptied source slot. Restore that source LAST.
        for (auto it = granted.rbegin(); it != granted.rend(); ++it) {
            log_pick_pc(&sd, LOG_TYPE_OTHER, -static_cast<int32>(it->amount), &sd.inventory.u.items_inventory[it->index]);
            sd.inventory.u.items_inventory[it->index] = it->before;
            sd.inventory_data[it->index] = it->data;
            clif_delitem(sd, it->index, it->amount, 0);
        }
        sd.inventory.u.items_inventory[request.server_index] = original;
        sd.inventory_data[request.server_index] = original_data;
        sd.weight = original_weight;
        sd.last_addeditem_index = original_last_added;
        log_pick_pc(&sd, LOG_TYPE_OTHER, mode.consume, &sd.inventory.u.items_inventory[request.server_index]);
        clif_additem(&sd, request.server_index, mode.consume, 0);
        clif_updatestatus(sd, SP_WEIGHT);
        pc_show_questinfo(&sd);
        ShowError("Rune Tablet: decomposition delivery rolled back for character %d, source %u.\n",
            sd.status.char_id, request.item_id);
        return "Rune Tablet: delivery failed; your inventory was restored.";
    }
    // No achievement/quest condition scripts ran while the batch was partial.
    for (const auto& result : results)
        if (result.amount)
            achievement_update_objective(&sd, AG_GET_ITEM, 1, itemdb_search(result.id)->value_sell);
    pc_show_questinfo(&sd); // Also refresh when all optional outputs missed.
    ShowInfo("Rune Tablet: character %d decomposed %u x %u; policy %s.\n",
        sd.status.char_id, request.item_id, static_cast<uint32>(mode.consume), fixed ? "fixed" : "local-uniform");
    return nullptr;
}

uint8 activate(map_session_data& sd, uint16 category, uint32 id, bool is_set) {
	auto record = rune_db.find(id);
	if (!record || record->category != category || record->is_set != is_set)
		return 6; // MSI_RUNESYSTEM_INVALID_ID
	if (owned(sd, *record))
		return 1; // Already active: repeated packets cannot charge twice.
	for (const auto& cost : record->materials)
		if (!item_db.exists(cost.id)) {
			ShowWarning("Rune Tablet: item %u for rune %u is absent from the item database.\n", cost.id, id);
			return 11;
		}
	std::vector<rune_tablet_logic::slot> inventory;
	inventory.reserve(MAX_INVENTORY);
	for (int i = 0; i < MAX_INVENTORY; ++i) {
		const auto& item = sd.inventory.u.items_inventory[i];
		inventory.push_back({item.nameid, item.amount > 0 ? static_cast<uint32>(item.amount) : 0,
			sd.inventory_data[i] != nullptr && eligible(item)});
	}
	auto plan = rune_tablet_logic::plan(record->materials, inventory);
	if (!plan)
		return 3; // MSI_RUNESYSTEM_NOT_ENOUGH_ITEM
	// No scripts, yields or equipment removals between validation and debit.
	// pc_delitem's failure conditions have all been checked by the plan above.
	for (const auto& debit : *plan) {
		if (pc_delitem(&sd, static_cast<int32>(debit.index), static_cast<int32>(debit.amount), 0, 0, LOG_TYPE_OTHER, false)) {
			ShowError("Rune Tablet: validated debit failed for character %d, rune %u.\n", sd.status.char_id, id);
			return 11;
		}
	}
	// Existing character registry persistence handles logout/autosave. Value 1
	// denotes activation at upgrade level zero.
	if (!pc_setregistry(&sd, progress_key(*record), 1)) {
		ShowError("Rune Tablet: registry write failed for character %d, rune %u.\n", sd.status.char_id, id);
		return 11;
	}
	status_calc_pc(&sd, SCO_NONE);
	pc_show_questinfo(&sd);
	return 0;
}

// Returns an error message only for an unprocessed attempt; processed failed
// rolls are successful protocol operations with a changed failure count.
const char* upgrade(map_session_data& sd, uint16 category, uint32 id, int64& next) {
    auto record = rune_db.find(id);
    if (!record || !record->is_set || record->category != category || !owned(sd, *record))
        return "Rune Tablet: invalid or inactive set.";
    const t_tick now = gettick();
    if (now < sd.rune_tablet_upgrade_after)
        return "Rune Tablet: wait one second before another upgrade attempt.";
    const auto key = progress_key(*record);
    const int64 before = pc_readregistry(&sd, key);
    const int current = rune_tablet_logic::level(before);
    if (current >= 15)
        return "Rune Tablet: this set has already reached the maximum level (+15).";
    auto rule = record->upgrades.find(static_cast<uint16>(current + 1));
    if (rule == record->upgrades.end())
        return "Rune Tablet: upgrade data for this level is missing.";
    if (rune_tablet_logic::failures(before) == 65535)
        return "Rune Tablet: failure counter exhausted; check your progress before continuing.";
    // No materials may be consumed for a table that can never succeed.
    if (!rule->second.chance && !rule->second.failure_increment)
        return "Rune Tablet: this set has no upgrade chance available.";
    for (const auto& cost : rule->second.materials)
        if (!item_db.exists(cost.id))
            return "Rune Tablet: an upgrade material is missing from the item database.";
    std::vector<rune_tablet_logic::slot> inventory;
    inventory.reserve(MAX_INVENTORY);
    for (int i = 0; i < MAX_INVENTORY; ++i) {
        const auto& item = sd.inventory.u.items_inventory[i];
        inventory.push_back({item.nameid, item.amount > 0 ? static_cast<uint32>(item.amount) : 0,
            sd.inventory_data[i] != nullptr && eligible(item)});
    }
    const auto plan = rune_tablet_logic::plan(rule->second.materials, inventory);
    if (!plan)
        return "Rune Tablet: you do not have the required upgrade materials.";
    next = rune_tablet_logic::upgrade_outcome(before, rule->second.chance,
        rule->second.failure_increment, rnd_value<uint32>(0, rune_tablet_logic::upgrade_probability_scale - 1));
    if (!next)
        return "Rune Tablet: invalid upgrade progress or exhausted counter.";
    // This session guard suppresses rapid duplicate packets; the native request
    // has no transaction ID, so later requests necessarily represent new attempts.
    sd.rune_tablet_upgrade_after = now + 1000;
    // Commit both progress fields together before debit. pc_setregistry only
    // fails when registries are unavailable; no scripts/yields occur here.
    if (!pc_setregistry(&sd, key, next))
        return "Rune Tablet: could not save the upgrade progress.";
    struct removed_material { item original; int32 amount; };
    std::vector<removed_material> removed;
    removed.reserve(plan->size());
    for (const auto& debit : *plan) {
        item original = sd.inventory.u.items_inventory[debit.index];
        if (pc_delitem(&sd, static_cast<int32>(debit.index), static_cast<int32>(debit.amount), 0, 0, LOG_TYPE_OTHER, false)) {
            // Defensive rollback: the debit planner already checks all current
            // pc_delitem failure conditions; reserved inventory space is freed.
            pc_setregistry(&sd, key, before);
            for (auto& entry : removed)
                if (pc_additem(&sd, &entry.original, entry.amount, LOG_TYPE_OTHER) != ADDITEM_SUCCESS)
                    ShowError("Rune Tablet: rollback item %u failed for character %d.\n", entry.original.nameid, sd.status.char_id);
            ShowError("Rune Tablet: validated upgrade debit failed for character %d, set %u.\n", sd.status.char_id, id);
            pc_show_questinfo(&sd);
            return "Rune Tablet: internal error while consuming materials; check the server log.";
        }
        removed.push_back({original, static_cast<int32>(debit.amount)});
    }
    if (rune_tablet_logic::level(next) != current && pc_readregistry(&sd, equipped_key()) == id)
        status_calc_pc(&sd, SCO_NONE);
    pc_show_questinfo(&sd);
    ShowInfo("Rune Tablet: character %d upgraded set %u: level %d -> %d, failures %d -> %d.\n",
        sd.status.char_id, id, current, rune_tablet_logic::level(next),
        rune_tablet_logic::failures(before), rune_tablet_logic::failures(next));
    return nullptr;
}

template<size_t N>
void send(map_session_data& sd, const std::array<uint8_t, N>& packet) {
	static_assert(N <= 32767, "Packet exceeds protocol length limit");
	clif_send(packet.data(), static_cast<int32>(N), &sd, SELF);
}

void send_progress(map_session_data& sd, uint16 category, bool is_set) {
	using namespace rune_tablet_wire;
	auto header = empty_list(is_set ? set_list : fragment_list, category);
	std::vector<uint8> packet(header.begin(), header.end());
	uint16 count = 0;
	for (const auto& entry : rune_db) {
		const auto& record = *entry.second;
		if (record.category != category || record.is_set != is_set || !owned(sd, record))
			continue;
		const size_t offset = packet.size();
		if (offset + (is_set ? 8 : 4) > 32767) {
			ShowError("Rune Tablet: progress list exceeds packet limit.\n");
			return;
		}
		packet.resize(offset + (is_set ? 8 : 4), 0);
		put32(packet.data() + offset, record.id);
		if (is_set) {
			const auto progress = pc_readregistry(&sd, progress_key(record));
			put16(packet.data() + offset + 4, static_cast<uint16>(rune_tablet_logic::level(progress)));
			put16(packet.data() + offset + 6, static_cast<uint16>(rune_tablet_logic::failures(progress)));
		}
		++count;
	}
	put16(packet.data() + 2, static_cast<uint16>(packet.size()));
	put16(packet.data() + 7, count);
	clif_send(packet.data(), static_cast<int32>(packet.size()), &sd, SELF);
}

#if PACKETVER_MAIN_NUM >= 20241016
// 0x0c15: every set of the category that has collection rewards, with the
// last claimed slot (list header like 0x0bcc, 5 bytes per entry).
void send_rewards(map_session_data& sd, uint16 category) {
	using namespace rune_tablet_wire;
	auto header = empty_list(reward_list, category);
	std::vector<uint8> packet(header.begin(), header.end());
	uint16 count = 0;
	for (const auto& entry : rune_db) {
		const auto& record = *entry.second;
		if (record.category != category || !record.is_set || !has_rewards(record))
			continue;
		const int64 claimed = pc_readregistry(&sd, reward_key(record));
		if (claimed < 0 || claimed > rune_tablet_logic::reward_slot_count || packet.size() + 5 > 32767)
			continue;
		const auto item = reward_entry(record.id, static_cast<uint8>(claimed));
		packet.insert(packet.end(), item.begin(), item.end());
		++count;
	}
	put16(packet.data() + 2, static_cast<uint16>(packet.size()));
	put16(packet.data() + 7, count);
	clif_send(packet.data(), static_cast<int32>(packet.size()), &sd, SELF);
}

// 0x0c16: claim one slot. One unit per slot; reward boxes carry their own
// contents. Nothing is given when the item does not fit (official guide).
uint8 grant_reward(map_session_data& sd, uint16 category, uint32 id, uint8 slot, uint8& claimed_out) {
	using namespace rune_tablet_logic;
	claimed_out = 0;
	auto record = rune_db.find(id);
	if (!record || !record->is_set || record->category != category || !has_rewards(*record))
		return reward_no_table;
	const int64 claimed = pc_readregistry(&sd, reward_key(*record));
	if (claimed < 0 || claimed > reward_slot_count)
		return reward_mismatch;
	claimed_out = static_cast<uint8>(claimed);
	const uint8 check = reward_check(record->rewards, static_cast<int>(claimed), slot,
		owned(sd, *record), active_fragments(sd, *record));
	if (check != reward_ok)
		return check;
	struct item reward{};
	reward.nameid = record->rewards[slot - 1];
	reward.identify = 1;
	if (!item_db.exists(reward.nameid))
		return reward_no_table;
	// Mark the slot first so a repeated packet cannot claim it twice; restore on failure.
	if (!pc_setregistry(&sd, reward_key(*record), slot))
		return reward_mismatch;
	switch (pc_additem(&sd, &reward, 1, LOG_TYPE_OTHER)) {
	case ADDITEM_SUCCESS:
		claimed_out = slot;
		ShowInfo("Rune Tablet: character %d claimed reward slot %u of set %u (item %u).\n",
			sd.status.char_id, slot, id, reward.nameid);
		return reward_ok;
	case ADDITEM_OVERWEIGHT:
		pc_setregistry(&sd, reward_key(*record), claimed);
		return reward_overweight;
	case ADDITEM_OVERITEM:
		pc_setregistry(&sd, reward_key(*record), claimed);
		return reward_no_space;
	case ADDITEM_OVERAMOUNT:
	case ADDITEM_STACKLIMIT:
		pc_setregistry(&sd, reward_key(*record), claimed);
		return reward_over_count;
	default:
		pc_setregistry(&sd, reward_key(*record), claimed);
		return reward_mismatch;
	}
}
#endif
}

void rune_tablet_init() {
	rune_db.load();
	decomposition_db.load();
}

void rune_tablet_final() {
	rune_db.clear();
	decomposition_db.clear();
}

// Called only during the normal status rebuild, after previous bonuses reset.
// No scripts or recursive status calculations: re-equipping cannot stack effects.
void rune_tablet_apply_bonuses(map_session_data& sd) {
	auto record = equipped_record(sd);
	if (!record)
		return;
	const auto count = active_fragments(sd, *record);
	const auto level = rune_tablet_logic::level(pc_readregistry(&sd, progress_key(*record)));
	const auto previous = sd.state.lr_flag;
	sd.state.lr_flag = LR_FLAG_NONE;
	for (const auto& effect : record->effects) {
		if (effect.special != rune_special::none)
			continue; // Applied by dedicated hooks, not the stat-bonus pool.
		const int32 value = rune_tablet_logic::effect_value(effect.rule, count, level);
		if (!value)
			continue;
		if (effect.has_parameter)
			pc_bonus2(&sd, effect.bonus, effect.parameter, value);
		else
			pc_bonus(&sd, effect.bonus, value);
	}
	sd.state.lr_flag = previous;
}

// Caller must already have filtered to a targeted magic kill (BF_MAGIC,
// status_isdead, !(INF_GROUND_SKILL|INF_SELF_SKILL)); battle_damage also
// requires actual HP loss before triggering this hook.
void rune_tablet_on_targeted_magic_kill(map_session_data& sd) {
	auto record = equipped_record(sd);
	if (!record)
		return;
	const auto count = active_fragments(sd, *record);
	const auto level = rune_tablet_logic::level(pc_readregistry(&sd, progress_key(*record)));
	int32 hp = 0, sp = 0;
	for (const auto& effect : record->effects) {
		switch (effect.special) {
		case rune_special::targeted_magic_kill_recovery_hp:
			hp += rune_tablet_logic::effect_value(effect.rule, count, level);
			break;
		case rune_special::targeted_magic_kill_recovery_sp:
			sp += rune_tablet_logic::effect_value(effect.rule, count, level);
			break;
		default:
			break;
		}
	}
	if (hp || sp)
		status_heal(&sd, hp, sp, battle_config.show_hp_sp_gain ? 3 : 1);
}

// battle_damage calls this only after actual HP loss on a surviving player.
// sc_start rolls the 2% chance; the status timer handles four recovery ticks.
void rune_tablet_on_hit(map_session_data& sd) {
	auto record = equipped_record(sd);
	if (!record)
		return;
	const auto count = active_fragments(sd, *record);
	const auto level = rune_tablet_logic::level(pc_readregistry(&sd, progress_key(*record)));
	for (const auto& effect : record->effects) {
		if (effect.special == rune_special::on_hit_sp_regen_chance
			&& count >= effect.rule.fragments && level >= effect.rule.min_level) {
			sc_start(&sd, &sd, SC_RUNE_TABLET_REGEN, 2, 4, 1000);
			return;
		}
	}
}

void rune_tablet_sync_equipped(map_session_data& sd) {
#if PACKETVER_MAIN_NUM >= 20260219
	if (!sd.vars_ok || !access_allowed(sd))
		return;
	auto record = equipped_record(sd);
	if (!record) {
		send(sd, rune_tablet_wire::equipped(0, 0, 0, 0));
		return;
	}
	send_progress(sd, record->category, false);
	send_progress(sd, record->category, true);
	send(sd, rune_tablet_wire::equipped(record->category, record->id,
		static_cast<uint16>(rune_tablet_logic::level(pc_readregistry(&sd, progress_key(*record)))),
		active_fragments(sd, *record)));
#endif
}

bool rune_tablet_open(map_session_data& sd, bool from_npc) {
#if PACKETVER_MAIN_NUM >= 20230802
	if (sd.state.rune_tablet_open) {
		clif_displaymessage(sd.fd, "Rune Tablet is already open. Close the window before opening it again.");
		return true;
	}
	if (!sd.vars_ok || !access_allowed(sd)) {
		clif_displaymessage(sd.fd, "Rune Tablet: not available yet.");
		return false;
	}
	// A Rune Stone NPC calls this from its own script, so its npc_id is set;
	// every other state that blocks acting still applies.
	const bool busy = from_npc ? (sd.chatID || pc_cant_act2(&sd)) : pc_cant_act(&sd);
	if (pc_isdead(&sd) || busy
		|| sd.state.mail_writing || sd.state.banking || sd.state.cashshop_open) {
		clif_displaymessage(sd.fd, "Rune Tablet: close other windows before opening it.");
		return false;
	}
	if (rune_db.empty()) {
		clif_displaymessage(sd.fd, "Rune Tablet: no definitions are loaded. Check db/re/rune_tablet_db.yml and the server log.");
		return false;
	}
	sd.state.rune_tablet_open = true;
	sd.state.rune_tablet_activation = true;
	clif_displaymessage(sd.fd, "Rune Tablet: Activate and Upgrade consume materials. A failed upgrade keeps the level and raises the chance of the next attempt.");
	send(sd, rune_tablet_wire::window(true));
	rune_tablet_sync_equipped(sd);
	return true;
#else
	clif_displaymessage(sd.fd, "Rune Tablet requires a compatible client (2023-08-02 or newer).");
	return false;
#endif
}

void rune_tablet_close(map_session_data& sd) {
#if PACKETVER_MAIN_NUM >= 20230802
	if (sd.state.rune_tablet_open) {
		sd.state.rune_tablet_open = false;
		sd.state.rune_tablet_activation = false;
		send(sd, rune_tablet_wire::window(false));
	}
#endif
}

void clif_parse_rune_tablet(int fd, map_session_data* sd) {
#if PACKETVER_MAIN_NUM >= 20230802
	if (!sd)
		return;
	const auto* data = static_cast<const uint8_t*>(RFIFOP(fd, 0));
	const uint16_t type = rune_tablet_wire::get16(data);
	if (type == rune_tablet_wire::window_state) {
		// UIRuneSystemWnd sends 1 from its constructor (0x814d40) and 0 from its
		// destructor (0x815870). Only the latter closes; do not echo a close packet.
		if (data[2] != 0)
			return;
		sd->state.rune_tablet_open = false;
		sd->state.rune_tablet_activation = false;
		return;
	}
	// Fixed lengths are validated by clif's packet dispatcher before this call.
	// The equipped-state packet requests a category even when the window is closed.
	// Read-only category refresh is allowed; all mutations require an open window.
	if ((!sd->state.rune_tablet_open && type != rune_tablet_wire::request_category)
		|| !sd->vars_ok || !access_allowed(*sd)) {
		ShowWarning("Rune Tablet: ignored packet 0x%04x from character %d (window/registry/GM state).\n", type, sd->status.char_id);
		return;
	}
	if (pc_isdead(sd) || sd->state.trading || sd->state.vending || sd->state.buyingstore
		|| sd->state.storage_flag || sd->state.mail_writing || sd->state.banking
		|| sd->state.cashshop_open || sd->npc_id || sd->npc_shopid) {
		rune_tablet_close(*sd);
		return;
	}
	using namespace rune_tablet_wire;
	if (type == request_category) {
		const uint16_t category = get16(data + 2);
		send_progress(*sd, category, false);
		send_progress(*sd, category, true);
#if PACKETVER_MAIN_NUM >= 20241016
		send_rewards(*sd, category);
#endif
		return;
	}
	if (sd->state.rune_tablet_activation && (type == activate_fragment || type == activate_set)) {
		const uint16 category = get16(data + 2);
		const uint32 id = get32(data + 4);
		const uint8 result = activate(*sd, category, id, type == activate_set);
		if (type == activate_set)
			send(*sd, operation<13>(set_result, result, category, id));
		else
			send(*sd, operation<9>(fragment_result, result, category, id));
		if (result == 0)
			rune_tablet_sync_equipped(*sd);
		return;
	}
    if (type == upgrade_set && sd->state.rune_tablet_activation) {
        const uint16 category = get16(data + 2);
        const uint32 id = get32(data + 4);
        int64 next = 0;
        if (const char* error = upgrade(*sd, category, id, next)) {
            // No invented native error code: close clears the pending UI.
            rune_tablet_close(*sd);
            clif_displaymessage(fd, error);
            return;
        }
        send(*sd, upgrade_state(0, category, id,
            static_cast<uint16>(rune_tablet_logic::level(next)),
            static_cast<uint16>(rune_tablet_logic::failures(next))));
        rune_tablet_sync_equipped(*sd);
        return;
    }
	// Clears the equipped set; false only if the registry write failed.
	auto clear_equipped = [sd, fd]() {
		if (pc_readregistry(sd, equipped_key()) == 0)
			return true;
		if (!pc_setregistry(sd, equipped_key(), 0)) {
			rune_tablet_close(*sd);
			clif_displaymessage(fd, "Rune Tablet: could not save the unequip.");
			return false;
		}
		status_change_end(sd, SC_RUNE_TABLET_REGEN);
		status_calc_pc(sd, SCO_NONE);
		return true;
	};
	if (type == equip) {
		const uint16 category = get16(data + 2);
		const uint32 id = get32(data + 4);
		if (id == 0) {
			// The native Unequip button (0x81ddcc) sends 0x0bd6 with ID 0; a
			// 0x0bd7 success with ID 0 clears the client's equipped set.
			if (clear_equipped())
				send(*sd, operation<9>(equip_result, 0, category, 0));
			return;
		}
		auto record = rune_db.find(id);
		if (!record || !record->is_set || record->category != category || !owned(*sd, *record)) {
			send(*sd, operation<9>(equip_result, 6, category, id));
			clif_displaymessage(fd, "Rune Tablet: invalid or inactive set.");
			return;
		}
		if (!equippable(*record)) {
			rune_tablet_close(*sd);
			clif_displaymessage(fd, "Rune Tablet: this set's effects are not verified yet. The equipped set was not changed.");
			return;
		}
		if (pc_readregistry(sd, equipped_key()) != id) {
			if (!pc_setregistry(sd, equipped_key(), id)) {
				rune_tablet_close(*sd);
				clif_displaymessage(fd, "Rune Tablet: could not save the equipped set.");
				return;
			}
			status_change_end(sd, SC_RUNE_TABLET_REGEN);
			status_calc_pc(sd, SCO_NONE);
		}
		send(*sd, operation<9>(equip_result, 0, category, id));
		return;
	}
	if (type == unequip) {
		// The native manager clears its single equipped set when ID is zero.
		auto previous = equipped_record(*sd);
		const uint16 category = previous ? previous->category : 0;
		if (!clear_equipped())
			return;
		send(*sd, operation<9>(unequip_result, 0, category, 0));
		return;
	}
#if PACKETVER_MAIN_NUM >= 20241016
	if (type == claim_reward) {
		const uint16 category = get16(data + 2);
		const uint32 id = get32(data + 4);
		uint8 claimed = 0;
		const uint8 result = grant_reward(*sd, category, id, data[8], claimed);
		send(*sd, reward_state(result, category, id, claimed));
		return;
	}
#endif
    if (type == decompose) {
        decomposition_request request{};
        const char* message = nullptr;
        if (!read_decomposition_request(data, 14, MAX_INVENTORY, request)) {
            message = "Rune Tablet: invalid decomposition request.";
        } else {
            const auto& source = sd->inventory.u.items_inventory[request.server_index];
            auto table = decomposition_db.find(request.item_id);
            if (!sd->inventory_data[request.server_index] || source.nameid != request.item_id
                || !eligible(source)) {
                message = "Rune Tablet: the decomposition item is missing, different, or protected.";
            } else if (!table) {
                message = "Rune Tablet: this item cannot be decomposed.";
            } else if (source.amount < table->modes[request.mode - 1].consume) {
                message = "Rune Tablet: not enough units in that stack for this mode.";
            } else if (!(message = decomposition_capacity_error(*sd, table->modes[request.mode - 1]))) {
                std::array<uint8_t, 51> response{};
                message = deliver_decomposition(*sd, request, table->modes[request.mode - 1], response);
                if (!message) {
                    send(*sd, response);
                    return;
                }
            }
            ShowInfo("Rune Tablet: decomposition preflight character %d, item %u, server index %u, mode %u; attempt rejected.\n",
                sd->status.char_id, request.item_id, request.server_index, request.mode);
        }
        // Rejected attempts close the pending UI operation without guessing
        // undocumented native error codes.
        rune_tablet_close(*sd);
        clif_displaymessage(fd, message);
        return;
    }
	// No native result is verified for an unimplemented operation. Closing
	// clears the pending UI state without claiming that the database is loading.
	rune_tablet_close(*sd);
	clif_displaymessage(fd, "Rune Tablet: this operation is not implemented yet; no items or progress were changed.");
	ShowInfo("Rune Tablet: probe rejected operation 0x%04x for character %d.\n", type, sd->status.char_id);
#endif
}
