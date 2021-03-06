/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * 
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "proc_mictx.hpp"
#include "tonesets.hpp"
#include "portapack_shared_memory.hpp"
#include "sine_table_int8.hpp"
#include "event_m4.hpp"

#include <cstdint>

void MicTXProcessor::execute(const buffer_c8_t& buffer){

	// This is called at 1536000/2048 = 750Hz
	
	if (!configured) return;
	
	audio_input.read_audio_buffer(audio_buffer);

	for (size_t i = 0; i<buffer.count; i++) {
		
		if (!play_beep) {
			sample = audio_buffer.p[i >> 6] >> 8;		// 1536000 / 64 = 24000
			sample = (sample * (int32_t)gain_x10) / 10;
			
			power += (sample < 0) ? -sample : sample;	// Power average for UI vu-meter
			
			if (!as) {
				as = divider;
				level_message.value = power / (divider / 4);				// Why ?
				shared_memory.application_queue.push(level_message);
				power = 0;
			} else {
				as--;
			}
		} else {
			if (beep_timer) {
				beep_timer--;
			} else {
				beep_timer = 76800;			// 50ms @ 1536000Hz
				if (beep_index == BEEP_TONES_NB) {
					configured = false;
					fm_delta = 0;			// Zero-out the IQ output for the rest of the buffer
					shared_memory.application_queue.push(txprogress_message);
				} else {
					beep_phase_inc = beep_deltas[beep_index];
					beep_index++;
				}
			}
			sample = sine_table_i8[(beep_phase & 0xFF000000U) >> 24];
			beep_phase += beep_phase_inc;
		}
		
		if (ctcss_enabled) {
			ctcss_sample = sine_table_i8[(ctcss_phase & 0xFF000000U) >> 24];
			sample_mixed = ((sample * 205) + (ctcss_sample * 50)) / 256;	// ~20%
			ctcss_phase += ctcss_phase_inc;
		} else {
			sample_mixed = sample;
		}
		
		// FM
		if (fm_delta) {
			delta = sample_mixed * fm_delta;
			
			phase += delta;
			sphase = phase + (64 << 24);

			re = (sine_table_i8[(sphase & 0xFF000000U) >> 24]);
			im = (sine_table_i8[(phase & 0xFF000000U) >> 24]);
		} else {
			re = 0;
			im = 0;
		}
		
		buffer.p[i] = {re, im};
	}
}

void MicTXProcessor::on_message(const Message* const msg) {
	const AudioTXConfigMessage config_message = *reinterpret_cast<const AudioTXConfigMessage*>(msg);
	const RequestSignalMessage request_message = *reinterpret_cast<const RequestSignalMessage*>(msg);
	
	switch(msg->id) {
		case Message::ID::AudioTXConfig:
			fm_delta = config_message.fm_delta * (0xFFFFFFULL / 1536000);
			gain_x10 = config_message.gain_x10;
			divider = config_message.divider;
			ctcss_enabled = config_message.ctcss_enabled;
			ctcss_phase_inc = config_message.ctcss_phase_inc;
			
			txprogress_message.done = true;

			play_beep = false;
			configured = true;
			break;
		
		case Message::ID::RequestSignal:
			if (request_message.signal == RequestSignalMessage::Signal::BeepRequest) {
				beep_index = 0;
				beep_timer = 0;
				play_beep = true;
			}
			break;

		default:
			break;
	}
}

int main() {
	EventDispatcher event_dispatcher { std::make_unique<MicTXProcessor>() };
	event_dispatcher.run();
	return 0;
}
