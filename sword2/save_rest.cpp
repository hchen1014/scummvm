/* Copyright (C) 1994-2003 Revolution Software Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Header$
 */

// ---------------------------------------------------------------------------
// SAVE_REST.CPP	save, restore & restart functions
//
// James 05feb97
//
// "Jesus Saves", but could he Restore or Restart? He can now...
//
// ---------------------------------------------------------------------------

#include "stdafx.h"
#include "sword2/driver/driver96.h"
#include "sword2/console.h"
#include "sword2/defs.h"
#include "sword2/function.h"	// for engine_logic, engine_graph, etc
#include "sword2/interpreter.h"	// for IR_CONT, etc
#include "sword2/layers.h"
#include "sword2/logic.h"
#include "sword2/memory.h"
#include "sword2/object.h"
#include "sword2/protocol.h"
#include "sword2/resman.h"
#include "sword2/router.h"
#include "sword2/save_rest.h"
#include "sword2/scroll.h"		// for Set_scrolling()
#include "sword2/sound.h"
#include "sword2/sword2.h"
#include "sword2/walker.h"

namespace Sword2 {

// max length of a savegame filename, including full path
#define	MAX_FILENAME_LEN 128

// savegame consists of header & global variables resource

#ifdef SCUMM_BIG_ENDIAN
// Quick macro to make swapping in-place easier to write
#define SWAP32(x)	x = SWAP_BYTES_32(x)

static void convertHeaderEndian(Sword2Engine::_savegameHeader &header) {
	int i;
	
	// _savegameHeader
	SWAP32(header.checksum);
	SWAP32(header.varLength);
	SWAP32(header.screenId);
	SWAP32(header.runListId);
	SWAP32(header.feet_x);
	SWAP32(header.feet_y);
	SWAP32(header.music_id);
	
	// _object_hub
	SWAP32(header.player_hub.type);
	SWAP32(header.player_hub.logic_level);
	for (i = 0; i < TREE_SIZE; i++) {
		SWAP32(header.player_hub.logic[i]);
		SWAP32(header.player_hub.script_id[i]);
		SWAP32(header.player_hub.script_pc[i]);
	}

	// Object_logic
	SWAP32(header.logic.looping);
	SWAP32(header.logic.pause);

	// Object_graphic
	SWAP32(header.graphic.type);
	SWAP32(header.graphic.anim_resource);
	SWAP32(header.graphic.anim_pc);

	// Object_mega
	SWAP32(header.mega.currently_walking);
	SWAP32(header.mega.walk_pc);
	SWAP32(header.mega.scale_a);
	SWAP32(header.mega.scale_b);
	SWAP32(header.mega.feet_x);
	SWAP32(header.mega.feet_y);
	SWAP32(header.mega.current_dir);
	SWAP32(header.mega.megaset_res);
}
#endif

// SAVE GAME

uint32 Sword2Engine::saveGame(uint16 slotNo, uint8 *desc) {
	mem *saveBufferMem;
	uint32 bufferSize;
	uint32 errorCode;

	// allocate the savegame buffer

	bufferSize = findBufferSize();
	saveBufferMem = memory->allocMemory(bufferSize, MEM_locked, UID_savegame_buffer);

	fillSaveBuffer(saveBufferMem, bufferSize, desc);

	// save it (hopefully no longer platform-specific)

	// save the buffer
	errorCode = saveData(slotNo, saveBufferMem->ad, bufferSize);

	// free the buffer

	memory->freeMemory(saveBufferMem);

	return errorCode;
}

// calculate size of required savegame buffer

uint32 Sword2Engine::findBufferSize(void) {
	// size of savegame header + size of global variables
	return sizeof(g_header) + res_man->fetchLen(1);
}

void Sword2Engine::fillSaveBuffer(mem *buffer, uint32 size, uint8 *desc) {
	uint8 *varsRes;

	// set up the g_header

	// 'checksum' gets filled in last of all

	// player's description of savegame
	strcpy(g_header.description, (char *) desc);

	// length of global variables resource
	g_header.varLength = res_man->fetchLen(1);

	// resource id of current screen file
	g_header.screenId = _thisScreen.background_layer_id;

	// resource id of current run-list
	g_header.runListId = g_logic->getRunList();

	// those scroll position control things
	g_header.feet_x = _thisScreen.feet_x;
	g_header.feet_y	= _thisScreen.feet_y;

	// id of currently looping music (or zero)
	g_header.music_id = _loopingMusicId;

	// object hub
	memcpy(&g_header.player_hub, res_man->openResource(CUR_PLAYER_ID) + sizeof(_standardHeader), sizeof(_object_hub));
	res_man->closeResource(CUR_PLAYER_ID);

	// logic, graphic & mega structures
	// copy the 4 essential player object structures into the header
	getPlayerStructures();

	// copy the header to the buffer

#ifdef SCUMM_BIG_ENDIAN
	convertHeaderEndian(g_header);
#endif

	// copy the header to the savegame buffer
	memcpy(buffer->ad, &g_header, sizeof(g_header));

	// copy the global variables to the buffer

	// open variables resource
	varsRes = res_man->openResource(1);

	// copy that to the buffer, following the header
	memcpy(buffer->ad + sizeof(g_header), varsRes, FROM_LE_32(g_header.varLength));

#ifdef SCUMM_BIG_ENDIAN
	uint32 *globalVars = (uint32 *) (buffer->ad + sizeof(g_header) + sizeof(_standardHeader));
	const uint numVars = (FROM_LE_32(g_header.varLength) - sizeof(_standardHeader)) / 4;

	for (uint i = 0; i < numVars; i++)
		globalVars[i] = SWAP_BYTES_32(globalVars[i]);
#endif

	// close variables resource
 	res_man->closeResource(1);

	// set the checksum & copy that to the buffer

	g_header.checksum = TO_LE_32(calcChecksum((buffer->ad) + sizeof(g_header.checksum), size - sizeof(g_header.checksum)));
 	memcpy(buffer->ad, &g_header.checksum, sizeof(g_header.checksum));
}

uint32 Sword2Engine::saveData(uint16 slotNo, uint8 *buffer, uint32 bufferSize) {
	char saveFileName[MAX_FILENAME_LEN];
	uint32 itemsWritten;
	SaveFile *out;
	SaveFileManager *mgr = g_system->get_savefile_manager();

	// construct filename
	sprintf(saveFileName, "%s.%.3d", _targetName, slotNo);
	
	if (!(out = mgr->open_savefile(saveFileName, getSavePath(), true))) {
		// error: couldn't open file
		delete mgr;
		return SR_ERR_FILEOPEN;
	}

	// write the buffer
	itemsWritten = out->write(buffer, bufferSize);

	delete out;
	delete mgr;

	// if we successfully wrote it all, then everything's ok
	if (itemsWritten == bufferSize)
		return SR_OK;

	// write failed for some reason (could be hard drive full)
	return SR_ERR_WRITEFAIL;
}

// RESTORE GAME

uint32 Sword2Engine::restoreGame(uint16 slotNo) {
	mem *saveBufferMem;
	uint32 bufferSize;
	uint32 errorCode;

	// allocate the savegame buffer

	bufferSize = findBufferSize();
	saveBufferMem = memory->allocMemory(bufferSize, MEM_locked, UID_savegame_buffer);

	// read the savegame file into our buffer

	// load savegame into buffer
	errorCode = restoreData(slotNo, saveBufferMem->ad, bufferSize);

	// if it was read in successfully, then restore the game from the
	// buffer & free the buffer

	if (errorCode == SR_OK)	{
		errorCode = restoreFromBuffer(saveBufferMem, bufferSize);

		// Note that the buffer has been freed inside
		// restoreFromBuffer, in order to clear it from memory before
		// loading in the new screen & runlist
	} else {
		// because restoreFromBuffer would have freed it
		memory->freeMemory(saveBufferMem);
	}

	return errorCode;
}

uint32 Sword2Engine::restoreData(uint16 slotNo, uint8 *buffer, uint32 bufferSize) {
	char saveFileName[MAX_FILENAME_LEN];
	SaveFile *in;
	SaveFileManager *mgr = g_system->get_savefile_manager();
 	uint32 itemsRead;

	// construct filename
	sprintf(saveFileName, "%s.%.3d", _targetName, slotNo);

	if (!(in = mgr->open_savefile(saveFileName, getSavePath(), false))) {
		// error: couldn't open file
		delete mgr;
		return SR_ERR_FILEOPEN;
	}

	// read savegame into the buffer
	itemsRead = in->read(buffer, bufferSize);

	if (itemsRead == bufferSize) {
		// We successfully read it all
		delete in;
		delete mgr;
		return SR_OK;
	} else {
		// We didn't get all of it.

/*
		// There is no ioFailed() function in SaveFile yet, I think.
		if (in->ioFailed()) {
			delete in;
			delete mgr;
			return SR_ERR_READFAIL;
		}
*/

		delete in;
		delete mgr;
		// error: incompatible save-data - can't use!
		return SR_ERR_INCOMPATIBLE;
	}
}

uint32 Sword2Engine::restoreFromBuffer(mem *buffer, uint32 size) {
	uint8 *varsRes;
	int32 pars[2];

	// get a copy of the header from the savegame buffer
	memcpy(&g_header, buffer->ad, sizeof(g_header));

#ifdef SCUMM_BIG_ENDIAN
	convertHeaderEndian(g_header);
#endif

	// Calc checksum & check that aginst the value stored in the header

	if (g_header.checksum != calcChecksum(buffer->ad + sizeof(g_header.checksum), size - sizeof(g_header.checksum))) {
		memory->freeMemory(buffer);

		// error: incompatible save-data - can't use!
		return SR_ERR_INCOMPATIBLE;
	}

	// check savegame against length of current global variables resource
	// This would most probably be trapped by the checksum test anyway,
	// but it doesn't do any harm to check this as well

	// Note that during development, earlier savegames will often be
	// shorter than the current expected length

	// if header contradicts actual current size of global variables
	if (g_header.varLength != res_man->fetchLen(1)) {
		memory->freeMemory(buffer);

		// error: incompatible save-data - can't use!
		return SR_ERR_INCOMPATIBLE;
	}

	// clean out system

	// trash all resources from memory except player object & global
	// variables
	res_man->killAll(false);

	// clean out the system kill list (no more objects to kill)
	g_logic->resetKillList();
	
	// get player character data from savegame buffer

	// object hub is just after the standard header 
	memcpy(res_man->openResource(CUR_PLAYER_ID) + sizeof(_standardHeader), &g_header.player_hub, sizeof(_object_hub));

	res_man->closeResource(CUR_PLAYER_ID);

	// fill in the 4 essential player object structures from the header
	putPlayerStructures();

	// get variables resource from the savegame buffer	

	// open variables resource
	varsRes = res_man->openResource(1);

	// copy that to the buffer, following the header
	memcpy(varsRes, buffer->ad + sizeof(g_header), g_header.varLength );

#ifdef SCUMM_BIG_ENDIAN
	uint32 *globalVars = (uint32 *) (varsRes + sizeof(_standardHeader));
	const uint numVars = (g_header.varLength - sizeof(_standardHeader)) / 4;

	for (uint i = 0; i < numVars; i++)
		globalVars[i] = SWAP_BYTES_32(globalVars[i]);
#endif

	// close variables resource
 	res_man->closeResource(1);

	// free it now, rather than in RestoreGame, to unblock memory before
	// new screen & runlist loaded
	memory->freeMemory(buffer);

	pars[0] = g_header.screenId;
	pars[1] = 1;
	g_logic->fnInitBackground(pars);

	// So palette not restored immediately after control panel - we want to
	// fade up instead!
	_thisScreen.new_palette = 99;

	// these need setting after the defaults get set in fnInitBackground
	// remember that these can change through the game, so need saving &
	// restoring too
	_thisScreen.feet_x = g_header.feet_x;
	_thisScreen.feet_y = g_header.feet_y;

	// start the new run list
	g_logic->expressChangeSession(g_header.runListId);

	// Force in the new scroll position, so unsightly scroll-catch-up does
	// not occur when screen first draws after returning from restore panel

	// set '_thisScreen's record of player position
	// - ready for setScrolling()

	_thisScreen.player_feet_x = g_header.mega.feet_x;
	_thisScreen.player_feet_y = g_header.mega.feet_y;

	// if this screen is wide, recompute the scroll offsets now
	if (_thisScreen.scroll_flag)
		setScrolling();

	// Any music required will be started after we've returned from
	// Restore_control() - see System_menu() in mouse.cpp!

	_loopingMusicId = g_header.music_id;

	// Write to walkthrough file (zebug0.txt)

	debug(5, "RESTORED GAME \"%s\"", g_header.description);

	// game restored ok
	return SR_OK;
}

// GetSaveDescription - PC version...

uint32 Sword2Engine::getSaveDescription(uint16 slotNo, uint8 *description) {
	char saveFileName[MAX_FILENAME_LEN];
	_savegameHeader dummy;
	SaveFile *in;
	SaveFileManager *mgr = g_system->get_savefile_manager();

	// construct filename
	sprintf(saveFileName, "%s.%.3d", _targetName, slotNo);

	if (!(in = mgr->open_savefile(saveFileName, getSavePath(), false))) {
		// error: couldn't open file
		delete mgr;
		return SR_ERR_FILEOPEN;
	}

	// read header
	in->read(&dummy, sizeof(_savegameHeader));
	delete in;
	delete mgr;

	strcpy((char *) description, dummy.description);
	return SR_OK;
}

bool Sword2Engine::saveExists(uint16 slotNo) {
	char saveFileName[MAX_FILENAME_LEN];
	SaveFileManager *mgr = g_system->get_savefile_manager();
	SaveFile *in;

	// construct filename
	sprintf(saveFileName, "%s.%.3d", _targetName, slotNo);

	if (!(in = mgr->open_savefile(saveFileName, getSavePath(), false))) {
		delete mgr;
		return false;
	}

	delete in;
	delete mgr;

	return true;
}

void Sword2Engine::getPlayerStructures(void) {
	 // request the player object structures which need saving

	// script no. 7 - 'george_savedata_request' calls fnPassPlayerSaveData

	uint32 null_pc = 7;
 	char *raw_script_ad;
	_standardHeader *head;

	head = (_standardHeader*) res_man->openResource(CUR_PLAYER_ID);

	if (head->fileType != GAME_OBJECT)
		error("incorrect CUR_PLAYER_ID=%d", CUR_PLAYER_ID);

	raw_script_ad = (char *) head;
	g_logic->runScript(raw_script_ad, raw_script_ad, &null_pc);
	res_man->closeResource(CUR_PLAYER_ID);
}

void Sword2Engine::putPlayerStructures(void) {
	// fill out the player object structures from the savegame structures
	// also run the appropriate scripts to set up george's anim tables &
	// walkdata, and nico's anim tables

	uint32 null_pc;
 	char *raw_script_ad;
	_standardHeader *head;

	head = (_standardHeader*) res_man->openResource(CUR_PLAYER_ID);

	if (head->fileType != GAME_OBJECT)
		error("incorrect CUR_PLAYER_ID=%d", CUR_PLAYER_ID);

	raw_script_ad = (char *) head;

	// script no. 8 - 'george_savedata_return' calls fnGetPlayerSaveData

	null_pc = 8;
	g_logic->runScript(raw_script_ad, raw_script_ad, &null_pc);

	// script no. 14 - 'set_up_nico_anim_tables'

	null_pc = 14;
	g_logic->runScript(raw_script_ad, raw_script_ad, &null_pc);

	// which megaset was the player at the time of saving?

	switch (g_header.mega.megaset_res) {
	case 36:		// GeoMega:
		null_pc = 9;	// script no.9	- 'player_is_george'
		break;
	case 2003:		// GeoMegaB:
		null_pc = 13;	// script no.13 - 'player_is_georgeB'
		break;
	case 1366:		// NicMegaA:
		null_pc = 11;	// script no.11 - 'player_is_nicoA'
		break;
	case 1437:		// NicMegaB:
		null_pc = 12;	// script no.12 - 'player_is_nicoB'
		break;
	case 1575:		// NicMegaC:
		null_pc = 10;	// script no.10 - 'player_is_nicoC'
		break;
	}

	g_logic->runScript(raw_script_ad, raw_script_ad, &null_pc);
	res_man->closeResource(CUR_PLAYER_ID);
}

uint32 Sword2Engine::calcChecksum(uint8 *buffer, uint32 size) {
	uint32 total = 0;

	for (uint32 pos = 0; pos < size; pos++)
		total += buffer[pos];

	return total;
}

int32 Logic::fnPassPlayerSaveData(int32 *params) {
	// copies the 4 essential player structures into the savegame header
	// - run script 7 of player object to request this

	// remember, we cannot simply read a compact any longer but instead
	// must request it from the object itself

	// params:	0 pointer to object's logic structure
	//		1 pointer to object's graphic structure
	//		2 pointer to object's mega structure

	// copy from player object to savegame header

	memcpy(&_vm->g_header.logic, (uint8 *) params[0], sizeof(Object_logic));
	memcpy(&_vm->g_header.graphic, (uint8 *) params[1], sizeof(Object_graphic));
	memcpy(&_vm->g_header.mega, (uint8 *) params[2], sizeof(Object_mega));

	// makes no odds
	return IR_CONT;
}

int32 Logic::fnGetPlayerSaveData(int32 *params) {
	// reverse of fnPassPlayerSaveData
	// - run script 8 of player object

	// params:	0 pointer to object's logic structure
	//		1 pointer to object's graphic structure
	//		2 pointer to object's mega structure

	Object_logic *ob_logic = (Object_logic *) params[0];
	Object_graphic *ob_graphic = (Object_graphic *) params[1];
	Object_mega *ob_mega = (Object_mega *) params[2];

	int32 pars[3];

	// copy from savegame header to player object

	memcpy((uint8 *) ob_logic, &_vm->g_header.logic, sizeof(Object_logic));
	memcpy((uint8 *) ob_graphic, &_vm->g_header.graphic, sizeof(Object_graphic));
	memcpy((uint8 *) ob_mega, &_vm->g_header.mega, sizeof(Object_mega));

 	// any walk-data must be cleared - the player will be set to stand if
	// he was walking when saved

	// if the player was walking when game was saved
	if (ob_mega->currently_walking) {
		// clear the flag
		ob_mega->currently_walking = 0;

		// pointer to object's graphic structure
		pars[0] = (int32) ob_graphic;

		// pointer to object's mega structure
		pars[1] = (int32) ob_mega;

		// target direction
		pars[2] = ob_mega->current_dir;

		// set player to stand
		fnStand(pars);

		// reset looping flag (which would have been '1' during fnWalk)
		ob_logic->looping = 0;
	}

	// makes no odds
	return IR_CONT;
}

} // End of namespace Sword2
