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

#include "receiver_model.hpp"

#include "spectrum_color_lut.hpp"

#include "ui_receiver.hpp"
#include "ui_font_fixed_8x16.hpp"
#include "recent_entries.hpp"

namespace ui {

#define SCAN_SLICE_WIDTH	2500000				// Scan slice bandwidth
#define SCAN_BIN_NB			256					// FFT power bins (skip 4 at center, 2*6 on sides)
#define SCAN_BIN_NB_NO_DC	(SCAN_BIN_NB - 16)	// Bins after trimming
#define SCAN_BIN_WIDTH		(SCAN_SLICE_WIDTH / SCAN_BIN_NB)

#define DETECT_DELAY		5	// In 100ms units
#define RELEASE_DELAY		6

struct ScannerRecentEntry {
	using Key = rf::Frequency;
	
	static constexpr Key invalid_key = 0xffffffff;
	
	rf::Frequency frequency;
	uint32_t duration { 0 };	// In 100ms units
	std::string time { "" };

	ScannerRecentEntry(
	) : ScannerRecentEntry { 0 }
	{
	}
	
	ScannerRecentEntry(
		const rf::Frequency frequency
	) : frequency { frequency }
	{
	}

	Key key() const {
		return frequency;
	}
	
	void set_time(std::string& new_time) {
		time = new_time;
	}
	
	void set_duration(uint32_t new_duration) {
		duration = new_duration;
	}
};

using ScannerRecentEntries = RecentEntries<ScannerRecentEntry>;

class ScannerView : public View {
public:
	ScannerView(NavigationView& nav);
	~ScannerView();
	
	ScannerView(const ScannerView&) = delete;
	ScannerView(ScannerView&&) = delete;
	ScannerView& operator=(const ScannerView&) = delete;
	ScannerView& operator=(ScannerView&&) = delete;
	
	void on_show() override;
	void on_hide() override;
	void focus() override;
	
	std::string title() const override { return "Close Call"; };

private:
	NavigationView& nav_;

	const Style style_grey {		// For informations and lost signal
		.font = font::fixed_8x16,
		.background = Color::black(),
		.foreground = Color::grey(),
	};
	
	const Style style_locked {
		.font = font::fixed_8x16,
		.background = Color::black(),
		.foreground = Color::green(),
	};
	
	struct slice_t {
		rf::Frequency center_frequency;
		uint8_t max_power;
		int16_t max_index;
		uint8_t power;
		int16_t index;
	} slices[32];
	
	ChannelSpectrumFIFO* fifo { nullptr };
	rf::Frequency f_min { 0 }, f_max { 0 };
	uint8_t detect_timer { 0 }, release_timer { 0 }, timing_div { 0 };
	uint8_t overall_power_max { 0 };
	uint32_t mean_power { 0 }, mean_acc { 0 };
	uint32_t duration { 0 };
	uint32_t power_threshold { 80 };	// Todo: Put this in persistent / settings
	rf::Frequency slice_start { 0 };
	uint8_t slices_nb { 0 };
	uint8_t slice_counter { 0 };
	int16_t last_bin { 0 };
	Coord last_pos { 0 };
	rf::Frequency scan_span { 0 }, resolved_frequency { 0 };
	uint16_t locked_bin { 0 };
	uint8_t scan_counter { 0 };
	bool locked { false };
	
	void on_channel_spectrum(const ChannelSpectrum& spectrum);
	void on_range_changed();
	void do_detection();
	void on_lna_changed(int32_t v_db);
	void on_vga_changed(int32_t v_db);
	void do_timers();
	
	const RecentEntriesColumns columns { {
		{ "Frequency", 9 },
		{ "Time", 8 },
		{ "Duration", 11 }
	} };
	ScannerRecentEntries recent { };
	RecentEntriesView<RecentEntries<ScannerRecentEntry>> recent_entries_view { columns, recent };
	
	Labels labels {
		{ { 1 * 8, 0 }, "Min:      Max:       LNA VGA", Color::light_grey() },
		{ { 1 * 8, 4 * 8 }, "Trig:   /255    Mean:   /255", Color::light_grey() },
		{ { 1 * 8, 6 * 8 }, "Slices:  /32      Rate:   Hz", Color::light_grey() },
		{ { 6 * 8, 10 * 8 }, "Timer  Status", Color::light_grey() },
		{ { 1 * 8, 25 * 8 }, "+/-4.9kHz:", Color::light_grey() },
		{ { 26 * 8, 25 * 8 }, "MHz", Color::light_grey() }
	};
	
	NumberField field_threshold {
		{ 6 * 8, 2 * 16 },
		3,
		{ 5, 255 },
		5,
		' '
	};
	 
	FrequencyField field_frequency_min {
		{ 1 * 8, 1 * 16 },
	};
	FrequencyField field_frequency_max {
		{ 11 * 8, 1 * 16 },
	};
	LNAGainField field_lna {
		{ 22 * 8, 1 * 16 }
	};
	VGAGainField field_vga {
		{ 26 * 8, 1 * 16 }
	};
	
	Text text_mean {
		{ 22 * 8, 2 * 16, 3 * 8, 16 },
		"---"
	};
	Text text_slices {
		{ 8 * 8, 3 * 16, 2 * 8, 16 },
		"--"
	};
	Text text_rate {
		{ 24 * 8, 3 * 16, 3 * 8, 16 },
		"---"
	};
	
	VuMeter vu_max {
		{ 1 * 8, 11 * 8 - 4, 3 * 8, 48 },
		16,
		false
	};
	ProgressBar progress_timers {
		{ 6 * 8, 12 * 8, 5 * 8, 16 }
	};
	Text text_infos {
		{ 13 * 8, 12 * 8, 15 * 8, 16 },
		"Listening"
	};
	Checkbox check_goto {
		{ 6 * 8, 15 * 8 },
		8,
		"On lock:",
		true
	};
	OptionsField options_goto {
		{ 17 * 8, 15 * 8 },
		7,
		{
			{ "Nothing", 0 },
			{ "NFM RX ", 1 },
			{ "POCSAG ", 2 }
		}
	};
	
	BigFrequency big_display {
		{ 4, 9 * 16, 28 * 8, 52 },
		0
	};
	
	Text text_approx {
		{ 11 * 8, 25 * 8, 11 * 8, 16 },
		"..."
	};
	
	MessageHandlerRegistration message_handler_spectrum_config {
		Message::ID::ChannelSpectrumConfig,
		[this](const Message* const p) {
			const auto message = *reinterpret_cast<const ChannelSpectrumConfigMessage*>(p);
			this->fifo = message.fifo;
		}
	};
	MessageHandlerRegistration message_handler_frame_sync {
		Message::ID::DisplayFrameSync,
		[this](const Message* const) {
			if( this->fifo ) {
				ChannelSpectrum channel_spectrum;
				while( fifo->out(channel_spectrum) ) {
					this->on_channel_spectrum(channel_spectrum);
				}
			}
			this->do_timers();
		}
	};
};

} /* namespace ui */
