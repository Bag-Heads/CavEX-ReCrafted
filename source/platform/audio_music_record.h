/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PLATFORM_AUDIO_MUSIC_RECORD_H
#define PLATFORM_AUDIO_MUSIC_RECORD_H

#include <stdbool.h>

/*
	Jukebox-record preempt logic [decompile 740c583:
	SoundManager.playStreaming]:

		playStreaming(record name, ...): when the sound system is
			loaded and the sound volume is not 0, a playing
				"streaming" source is stopped first. Then, for a non-null
					record name, a random entry for it is fetched from the
				streaming pool; when one exists and Beta's volume check
					passes, a playing "BgMusic" source is stopped and a new
					streaming source starts (as "streaming").








	i.e. INSERTING a record (a non-null name, this engine's audio_music_play_
	record) unconditionally stops whatever was sounding -- another record OR
	the background track -- and starts the new one; EJECTING (a null name,
	this engine's audio_music_stop_record) only stops a record if one was
	actually active, and never touches background music (Beta only ever
	calls playStreaming(null,...) from the jukebox eject path
	[RenderGlobal.playRecord], never from a background-music context).
	Background music itself never preempts a playing record either: Beta's
	playRandomMusicIfReady gates its ENTIRE body (including the countdown)
	on `!playing("streaming")` [audio_music_timer.h's `track_playing`
	parameter, which audio_music.c already feeds from the SAME track_active
	flag a record sets] -- so once a record is sounding, the background
	timer is simply frozen, never fires, and therefore never has anything to
	preempt.

	Pure + header-only (audio_music_timer.h / audio_music_ring.h precedent)
	so the PREEMPT DECISION is golden-testable without a real file, thread,
	or device; audio_music.c is the one caller, using the `stop_previous`
	output to skip an unnecessary stop+drain when nothing was actually
	sounding (e.g. inserting a record into a silent world costs no needless
	3ms Wii AESND drain sleep).
*/
enum audio_music_source {
	AUDIO_MUSIC_SOURCE_NONE = 0,
	AUDIO_MUSIC_SOURCE_BGMUSIC,
	AUDIO_MUSIC_SOURCE_RECORD,
};

/* current         - what is sounding through the shared voice right now.
   starting_record - true for an insert (audio_music_play_record), false
                      for an eject/stop (audio_music_stop_record).
   *out_stop_previous (may be NULL) receives whether the caller must stop
   whatever was previously active before proceeding. Returns the new
   source. */
static inline enum audio_music_source
audio_music_record_transition(enum audio_music_source current,
							  bool starting_record, bool* out_stop_previous) {
	if(starting_record) {
		/* [playStreaming, var1 != null branch] always stops whatever was
		   sounding -- streaming (unconditional, top of the method) AND
		   BgMusic (inside the var1!=null branch, once a real pool entry is
		   about to start). This engine has only ONE voice for both, so
		   "stop streaming" and "stop BgMusic" collapse into one action:
		   stop whatever that single voice currently holds, if anything. */
		if(out_stop_previous)
			*out_stop_previous = (current != AUDIO_MUSIC_SOURCE_NONE);
		return AUDIO_MUSIC_SOURCE_RECORD;
	}
	/* [playStreaming, var1 == null branch] only "stop streaming" ever runs;
	   BgMusic is never touched from this call shape. Stop only if a record
	   was the thing actually sounding -- leave background music (or plain
	   silence) exactly as it was otherwise. */
	bool was_record = (current == AUDIO_MUSIC_SOURCE_RECORD);
	if(out_stop_previous)
		*out_stop_previous = was_record;
	return was_record ? AUDIO_MUSIC_SOURCE_NONE : current;
}

#endif
