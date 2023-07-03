#include "hsd/pad.h"
#include "melee/player.h"
#include "melee/characters/zelda.h"
#include "ucf/pad_buffer.h"
#include "util/melee/pad.h"
#include <bit>
#include <cmath>

SHARED_DATA shared_pad_buffer data;

register Player *player asm("r31");

static bool check_ucf_sdrop(const port_pad_buffer &buffer)
{
	// Tilt intent algorithm designed by tauKhan
	const auto &prev_input = get_ucf_pad_buffer<-2>(buffer);
	const auto &current_input = get_ucf_pad_buffer<0>(buffer);
	const auto delta = current_input.stick.y - prev_input.stick.y;
	return delta * delta > 44 * 44;
}

static bool check_sdrop_up(const auto &buffer, const PlayerInput &input)
{
	// Must be -6125 or below along the rim, adjusted to prevent an ICs desync
	if (input.stick.y > FP(popo_to_nana(-.6125f)))
		return false;

	if (!is_rim_coord(input.stick))
		return false;

	// Only check speed on first frame
	if (buffer.sdrop_up_frames != 0)
		return true;

	return input.stick_y_hold_time < 2 && check_ucf_sdrop(buffer);
}


// Produce 1.0 cardinals when one axis is >= 0.9875 and the other is within
// [-SNAP_RANGE, SNAP_RANGE] and the raw major axis value is >= 80
[[gnu::noinline]] static void apply_cardinals(const vec2c &raw_stick, vec2 *stick)
{
	const s8 raw_x = raw_stick.x;
	const s8 raw_y = raw_stick.y;

	// We want to use bitwise operations for simplicity/code size, and there's
	// complications to using floats, so we'll treat them as signed integers.
	// Importantly, lt/gt operations will still be correct.
	const s32 SNAP_RANGE = std::bit_cast<s32>(0.075f); // (6 coords)
	const s32 THRESHOLD = std::bit_cast<s32>(0.9875f);
	const s32 SNAP_VALUE = std::bit_cast<s32>(1.0f);
	vec2i *stick_s32 = (vec2i *)stick;
	const s32 x = stick_s32->x;
	const s32 y = stick_s32->y;

	// Mask out the sign bit to take the absolute value
	if ((x & 0x7FFFFFFF) >= THRESHOLD && (y & 0x7FFFFFFF) <= SNAP_RANGE && std::abs(raw_x) >= 80) {
		// Use the original sign bit to assign ±1.0f appropriately
		*stick_s32 = {(x & 0x80000000) | SNAP_VALUE, 0};
	} else if ((y & 0x7FFFFFFF) >= THRESHOLD && (x & 0x7FFFFFFF) <= SNAP_RANGE && std::abs(raw_y) >= 80) {
		*stick_s32 = {0, (y & 0x80000000) | SNAP_VALUE};
	}
}

static bool should_apply_cardinals(const Player *player)
{
	switch (player->character_id) {
	case CID_Zelda:
		return player->action_state != AS_Zelda_SpecialHiStart;
	default:
		return true;
	}
}

static void gecko_entry()
{
	if (!Player_IsCPU(player)) {
		auto *buffer = &data.buffer[player->port];

		// Save to next entry in ring buffer
		const auto status = get_input<0>(player->port);
		buffer->index = (buffer->index + 1) & UCF_PAD_BUFFER_MASK;
		buffer->entries[buffer->index].stick = status.stick;

		if (should_apply_cardinals(player)) {
			apply_cardinals(status.stick, &player->input.stick);
			apply_cardinals(status.cstick, &player->input.cstick);
		}

		if (check_sdrop_up(*buffer, player->input))
			buffer->sdrop_up_frames++;
		else
			buffer->sdrop_up_frames = 0;
	}

	// Overwritten instruction
	register int analog_lr_neutral_time asm("r3");
	analog_lr_neutral_time = player->input.analog_lr_neutral_time;
	FORCE_WRITE(analog_lr_neutral_time);
}

GECKO_NAME("UCF Pad Buffer");
GECKO_INIT_PIC(UCF_PAD_BUFFER_INJECTION, gecko_entry, "r28");