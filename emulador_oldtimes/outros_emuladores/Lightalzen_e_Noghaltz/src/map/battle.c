// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../common/cbasetypes.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/showmsg.h"
#include "../common/ers.h"

#include "map.h"
#include "pc.h"
#include "status.h"
#include "skill.h"
#include "mob.h"
#include "itemdb.h"
#include "clif.h"
#include "pet.h"
#include "guild.h"
#include "party.h"
#include "battle.h"

int attr_fix_table[4][ELE_MAX][ELE_MAX];

struct Battle_Config battle_config;
static struct eri *delay_damage_ers; //For battle delay damage structures.

int battle_getcurrentskill(struct block_list *bl)
{	//Returns the current/last skill in use by this bl.
	struct unit_data *ud;

	if (bl->type == BL_SKILL) {
		struct skill_unit * su = (struct skill_unit*)bl;
		return su->group?su->group->skill_id:0;
	}
	ud = unit_bl2ud(bl);
	return ud?ud->skillid:0;
}

/*==========================================
 * Get random targetting enemy
 *------------------------------------------
 */
static int battle_gettargeted_sub(struct block_list *bl, va_list ap)
{
	struct block_list **bl_list;
	struct unit_data *ud;
	int target_id;
	int *c;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	target_id = va_arg(ap, int);

	if (bl->id == target_id)
		return 0;
	if (*c >= 24)
		return 0;

	ud = unit_bl2ud(bl);
	if (!ud) return 0;

	if (ud->target == target_id || ud->skilltarget == target_id) {
		bl_list[(*c)++] = bl;
		return 1;
	}
	return 0;	
}

struct block_list* battle_gettargeted(struct block_list *target)
{
	struct block_list *bl_list[24];
	int c = 0;
	nullpo_retr(NULL, target);

	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinrange(battle_gettargeted_sub, target, AREA_SIZE, BL_CHAR, bl_list, &c, target->id);
	if (c == 0 || c > 24)
		return NULL;
	return bl_list[rand()%c];
}


//Returns the id of the current targetted character of the passed bl. [Skotlex]
int battle_gettarget(struct block_list *bl)
{
	switch (bl->type)
	{
		case BL_PC:
			return ((struct map_session_data*)bl)->ud.target;
		case BL_MOB:
			return ((struct mob_data*)bl)->target_id;
		case BL_PET:
			return ((struct pet_data*)bl)->target_id;
		case BL_HOM:
			return ((struct homun_data*)bl)->ud.target;
	}
	return 0;
}

static int battle_getenemy_sub(struct block_list *bl, va_list ap)
{
	struct block_list **bl_list;
	struct block_list *target;
	int *c;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	target = va_arg(ap, struct block_list *);

	if (bl->id == target->id)
		return 0;
	if (*c >= 24)
		return 0;

	if (battle_check_target(target, bl, BCT_ENEMY) > 0) {
		bl_list[(*c)++] = bl;
		return 1;
	}
	return 0;	
}

// Picks a random enemy of the given type (BL_PC, BL_CHAR, etc) within the range given. [Skotlex]
struct block_list* battle_getenemy(struct block_list *target, int type, int range)
{
	struct block_list *bl_list[24];
	int c = 0;
	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinrange(battle_getenemy_sub, target, range, type, bl_list, &c, target);
	if (c == 0 || c > 24)
		return NULL;
	return bl_list[rand()%c];
}

// _??[WΜx
struct delay_damage {
	struct block_list *src;
	int target;
	int damage;
	int delay;
	unsigned short distance;
	unsigned short skill_lv;
	unsigned short skill_id;
	unsigned short dmg_lv;
	unsigned char attack_type;
};

int battle_delay_damage_sub (int tid, unsigned int tick, int id, int data)
{
	struct delay_damage *dat = (struct delay_damage *)data;
	struct block_list *target = map_id2bl(dat->target);
	if (target && dat && map_id2bl(id) == dat->src && target->prev != NULL && !status_isdead(target) &&
		target->m == dat->src->m && check_distance_bl(dat->src, target, dat->distance)) //Check to see if you haven't teleported. [Skotlex]
	{
		status_fix_damage(dat->src, target, dat->damage, dat->delay);
		if ((dat->dmg_lv == ATK_DEF || dat->damage > 0) && dat->attack_type)
		{
			if (!status_isdead(target))
				skill_additional_effect(dat->src,target,dat->skill_id,dat->skill_lv,dat->attack_type, tick);
			skill_counter_additional_effect(dat->src,target,dat->skill_id,dat->skill_lv,dat->attack_type,tick);
		}

	}
	ers_free(delay_damage_ers, dat);
	return 0;
}

int battle_delay_damage (unsigned int tick, struct block_list *src, struct block_list *target, int attack_type, int skill_id, int skill_lv, int damage, int dmg_lv, int ddelay)
{
	struct delay_damage *dat;
	nullpo_retr(0, src);
	nullpo_retr(0, target);

	if (!battle_config.delay_battle_damage) {
		status_fix_damage(src, target, damage, ddelay);
		if ((damage > 0 || dmg_lv == ATK_DEF) && attack_type)
		{
			if (!status_isdead(target))
				skill_additional_effect(src, target, skill_id, skill_lv, attack_type, gettick());
			skill_counter_additional_effect(src, target, skill_id, skill_lv, attack_type, gettick());
		}
		return 0;
	}
	dat = ers_alloc(delay_damage_ers, struct delay_damage);
	dat->src = src;
	dat->target = target->id;
	dat->skill_id = skill_id;
	dat->skill_lv = skill_lv;
	dat->attack_type = attack_type;
	dat->damage = damage;
	dat->dmg_lv = dmg_lv;
	dat->delay = ddelay;
	dat->distance = distance_bl(src, target)+10; //Attack should connect regardless unless you teleported.
	add_timer(tick, battle_delay_damage_sub, src->id, (int)dat);
	
	return 0;
}
/*==========================================
 * Does attribute fix modifiers. 
 * Added passing of the chars so that the status changes can affect it. [Skotlex]
 * Note: Passing src/target == NULL is perfectly valid, it skips SC_ checks.
 *------------------------------------------
 */
int battle_attr_fix(struct block_list *src, struct block_list *target, int damage,int atk_elem,int def_type, int def_lv)
{
	struct status_change *sc=NULL, *tsc=NULL;
	int ratio;
	
	if (src) sc = status_get_sc(src);
	if (target) tsc = status_get_sc(target);
	
	if (atk_elem < 0 || atk_elem >= ELE_MAX)
		atk_elem = rand()%ELE_MAX;

	if (def_type < 0 || def_type > ELE_MAX ||
		def_lv < 1 || def_lv > 4) {
		if (battle_config.error_log)
			ShowError("battle_attr_fix: unknown attr type: atk=%d def_type=%d def_lv=%d\n",atk_elem,def_type,def_lv);
		return damage;
	}

	ratio = attr_fix_table[def_lv-1][atk_elem][def_type];
	if (sc && sc->count)
	{
		if(sc->data[SC_VOLCANO].timer!=-1 && atk_elem == ELE_FIRE)
			ratio += enchant_eff[sc->data[SC_VOLCANO].val1-1];
		if(sc->data[SC_VIOLENTGALE].timer!=-1 && atk_elem == ELE_WIND)
			ratio += enchant_eff[sc->data[SC_VIOLENTGALE].val1-1];
		if(sc->data[SC_DELUGE].timer!=-1 && atk_elem == ELE_WATER)
			ratio += enchant_eff[sc->data[SC_DELUGE].val1-1];
	}
	if (tsc && tsc->count)
	{
		if(tsc->data[SC_ARMOR_ELEMENT].timer!=-1)
		{
			if (tsc->data[SC_ARMOR_ELEMENT].val1 == atk_elem)
				ratio -= tsc->data[SC_ARMOR_ELEMENT].val2;
			else
			if (tsc->data[SC_ARMOR_ELEMENT].val3 == atk_elem)
				ratio -= tsc->data[SC_ARMOR_ELEMENT].val4;
		}
		if(tsc->data[SC_SPIDERWEB].timer!=-1 && atk_elem == ELE_FIRE)
		{	// [Celest]
			damage <<= 1;
			status_change_end(target, SC_SPIDERWEB, -1);
		}
	}
	return damage*ratio/100;
}

/*==========================================
 * _??[W?Ε?IvZ
 *------------------------------------------
 */
int battle_calc_damage(struct block_list *src,struct block_list *bl,int damage,int div_,int skill_num,int skill_lv,int flag)
{
	struct map_session_data *sd = NULL;
	struct status_change *sc;
	struct status_change_entry *sci;

	nullpo_retr(0, bl);

	if (!damage)
		return 0;
	
	if (bl->type == BL_PC) {
		sd=(struct map_session_data *)bl;
		//Special no damage states
		if(flag&BF_WEAPON && sd->special_state.no_weapon_damage)
			damage -= damage*sd->special_state.no_weapon_damage/100;

		if(flag&BF_MAGIC && sd->special_state.no_magic_damage)
			damage -= damage*sd->special_state.no_magic_damage/100;

		if(flag&BF_MISC && sd->special_state.no_misc_damage)
			damage -= damage*sd->special_state.no_misc_damage/100;

		if(!damage) return 0;
	}
	
	if (skill_num == PA_PRESSURE)
		return damage; //This skill bypass everything else.

	sc = status_get_sc(bl);

	if((flag&(BF_MAGIC|BF_LONG)) == BF_LONG &&
		map_getcell(bl->m, bl->x, bl->y, CELL_CHKPNEUMA) &&
		skill_num != NPC_GUIDEDATTACK)
		return 0;
	
	if (sc && sc->count) {
		//First, sc_*'s that reduce damage to 0.
		if (sc->data[SC_SAFETYWALL].timer!=-1 && flag&BF_SHORT && skill_num != NPC_GUIDEDATTACK
		) {
			struct skill_unit_group *group = (struct skill_unit_group *)sc->data[SC_SAFETYWALL].val3;
			if (group) {
				if (--group->val2<=0)
					skill_delunitgroup(NULL,group,0);
				return 0;
			}
			status_change_end(bl,SC_SAFETYWALL,-1);
		}
	
		if(sc->data[SC_AUTOGUARD].timer != -1 && flag&BF_WEAPON &&
			!(skill_get_nk(skill_num)&NK_NO_CARDFIX_ATK) &&
			rand()%100 < sc->data[SC_AUTOGUARD].val2) {
			int delay;
			clif_skill_nodamage(bl,bl,CR_AUTOGUARD,sc->data[SC_AUTOGUARD].val1,1);
			// different delay depending on skill level [celest]
			if (sc->data[SC_AUTOGUARD].val1 <= 5)
				delay = 300;
			else if (sc->data[SC_AUTOGUARD].val1 > 5 && sc->data[SC_AUTOGUARD].val1 <= 9)
				delay = 200;
			else
				delay = 100;
			unit_set_walkdelay(bl, gettick(), delay, 1);

			if(sc->data[SC_SHRINK].timer != -1 && rand()%100<5*sc->data[SC_AUTOGUARD].val1)
				skill_blown(bl,src,skill_get_blewcount(CR_SHRINK,1));
			return 0;
		}

// -- moonsoul (chance to block attacks with new Lord Knight skill parrying)
//
		if(sc->data[SC_PARRYING].timer != -1 && flag&BF_WEAPON &&
			rand()%100 < sc->data[SC_PARRYING].val2) {
			clif_skill_nodamage(bl,bl,LK_PARRYING,sc->data[SC_PARRYING].val1,1);
			return 0;
		}
		
		if(sc->data[SC_DODGE].timer != -1 && !sc->opt1 &&
			(flag&BF_LONG || sc->data[SC_SPURT].timer != -1)
			&& rand()%100 < 20) {
			if (sd && pc_issit(sd)) pc_setstand(sd); //Stand it to dodge.
			clif_skill_nodamage(bl,bl,TK_DODGE,1,1);
			if (sc->data[SC_COMBO].timer == -1)
				sc_start4(bl, SC_COMBO, 100, TK_JUMPKICK, src->id, 1, 0, 2000);
			return 0;
		}

		if(sc->data[SC_HERMODE].timer != -1 && flag&BF_MAGIC)
			return 0;

		if(sc->data[SC_TATAMIGAESHI].timer != -1 && (flag&(BF_MAGIC|BF_LONG)) == BF_LONG)
			return 0;

		if(sc->data[SC_KAUPE].timer != -1 &&
			rand()%100 < sc->data[SC_KAUPE].val2 &&
			(src->type == BL_PC || !skill_num))
		{	//Kaupe only blocks all skills of players.
			clif_specialeffect(bl, 462, AREA);
			//Shouldn't end until Breaker's non-weapon part connects.
			if (skill_num != ASC_BREAKER || !(flag&BF_WEAPON))
				if (--sc->data[SC_KAUPE].val3 <= 0) //We make it work like Safety Wall, even though it only blocks 1 time.
					status_change_end(bl, SC_KAUPE, -1);
			return 0;
		}

		if ((sc->data[SC_UTSUSEMI].timer != -1 || sc->data[SC_BUNSINJYUTSU].timer != -1)
		&& 
			(flag&BF_WEAPON || (flag&(BF_MISC|BF_SHORT)) == (BF_MISC|BF_SHORT)) &&
			!(skill_get_nk(skill_num)&NK_NO_CARDFIX_ATK)
		)
		{
			if (sc->data[SC_UTSUSEMI].timer != -1) {
				clif_specialeffect(bl, 462, AREA);
				skill_blown (src, bl, sc->data[SC_UTSUSEMI].val3);
			};
			//Both need to be consumed if they are active.
			if (sc->data[SC_UTSUSEMI].timer != -1 &&
				--sc->data[SC_UTSUSEMI].val2 <= 0)
				status_change_end(bl, SC_UTSUSEMI, -1);
			if (sc->data[SC_BUNSINJYUTSU].timer != -1 &&
				--sc->data[SC_BUNSINJYUTSU].val2 <= 0)
				status_change_end(bl, SC_BUNSINJYUTSU, -1);
			return 0;
		}

		//Now damage increasing effects
		if(sc->data[SC_AETERNA].timer!=-1 && skill_num != PF_SOULBURN){
			damage<<=1;
			//Shouldn't end until Breaker's non-weapon part connects.
			if (skill_num != ASC_BREAKER || !(flag&BF_WEAPON))
				status_change_end( bl,SC_AETERNA,-1 );
		}

		//Finally damage reductions....
		if(sc->data[SC_ASSUMPTIO].timer != -1){
			if(map_flag_vs(bl->m))
				damage=damage*2/3; //Receive 66% damage
			else
				damage>>=1; //Receive 50% damage
		}

		if(sc->data[SC_DEFENDER].timer != -1 &&
			(flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
			damage=damage*(100-sc->data[SC_DEFENDER].val2)/100;

		if(sc->data[SC_ADJUSTMENT].timer != -1 &&
			(flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
			damage -= 20*damage/100;

		if(sc->data[SC_FOGWALL].timer != -1) {
			if(flag&BF_SKILL) //25% reduction
				damage -= 25*damage/100;
			else if ((flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
				damage >>= 2; //75% reduction
		}

		if(sc->data[SC_ARMOR].timer != -1 &&
			sc->data[SC_ARMOR].val3&flag &&
			sc->data[SC_ARMOR].val4&flag)
			//NPC_DEFENDER
			damage -= damage*sc->data[SC_ARMOR].val2/100;

		if(sc->data[SC_ENERGYCOAT].timer!=-1 && flag&BF_WEAPON){
			struct status_data *status = status_get_status_data(bl);
			int per = 100*status->sp / status->max_sp -1; //100% should be counted as the 80~99% interval
			per /=20; //Uses 20% SP intervals.
			//SP Cost: 1% + 0.5% per every 20% SP
			if (!status_charge(bl, 0, (10+5*per)*status->max_sp/1000))
				status_change_end( bl,SC_ENERGYCOAT,-1 );
			//Reduction: 6% + 6% every 20%
			damage -= damage * 6 * (1+per) / 100;
		}

		if(sc->data[SC_REJECTSWORD].timer!=-1 && flag&BF_WEAPON &&
			// Fixed the condition check [Aalye]
			(src->type!=BL_PC || (
				((TBL_PC *)src)->status.weapon == W_DAGGER ||
				((TBL_PC *)src)->status.weapon == W_1HSWORD ||
				((TBL_PC *)src)->status.weapon == W_2HSWORD
			))
		){
			if(rand()%100 < sc->data[SC_REJECTSWORD].val2){
				damage = damage*50/100;
				status_fix_damage(bl,src,damage,clif_damage(bl,src,gettick(),0,0,damage,0,0,0));
				clif_skill_nodamage(bl,bl,ST_REJECTSWORD,sc->data[SC_REJECTSWORD].val1,1);
				if((--sc->data[SC_REJECTSWORD].val3)<=0)
					status_change_end(bl, SC_REJECTSWORD, -1);
			}
		}

		//Finally Kyrie because it may, or not, reduce damage to 0.
		if(sc->data[SC_KYRIE].timer!=-1 && damage > 0){
			sci=&sc->data[SC_KYRIE];
			sci->val2-=damage;
			if(flag&BF_WEAPON || skill_num == TF_THROWSTONE){
				if(sci->val2>=0)
					damage=0;
				else
				  	damage=-sci->val2;
			}
			if((--sci->val3)<=0 || (sci->val2<=0) || skill_num == AL_HOLYLIGHT)
				status_change_end(bl, SC_KYRIE, -1);
		}

		if (!damage) return 0;

		//Probably not the most correct place, but it'll do here
		//(since battle_drain is strictly for players currently)
		if (sc->data[SC_BLOODLUST].timer != -1 && flag&BF_WEAPON && damage > 0 &&
			rand()%100 < sc->data[SC_BLOODLUST].val3)
			status_heal(src, damage*sc->data[SC_BLOODLUST].val4/100, 0, 3);

	}
	//SC effects from caster side. Currently none.
/*	
	sc = status_get_sc(src);
	if (sc && sc->count) {
	}
*/	
	if (battle_config.pk_mode && sd && bl->type == BL_PC && damage)
  	{
		if (flag & BF_SKILL) { //Skills get a different reduction than non-skills. [Skotlex]
			if (flag&BF_WEAPON)
				damage = damage * battle_config.pk_weapon_damage_rate/100;
			if (flag&BF_MAGIC)
				damage = damage * battle_config.pk_magic_damage_rate/100;
			if (flag&BF_MISC)
				damage = damage * battle_config.pk_misc_damage_rate/100;
		} else { //Normal attacks get reductions based on range.
			if (flag & BF_SHORT)
				damage = damage * battle_config.pk_short_damage_rate/100;
			if (flag & BF_LONG)
				damage = damage * battle_config.pk_long_damage_rate/100;
		}
		if(!damage) damage  = 1;
	}

	if(battle_config.skill_min_damage && damage > 0 && damage < div_)
	{
		if ((flag&BF_WEAPON && battle_config.skill_min_damage&1)
			|| (flag&BF_MAGIC && battle_config.skill_min_damage&2)
			|| (flag&BF_MISC && battle_config.skill_min_damage&4)
		)
			damage = div_;
	}

	if( bl->type == BL_MOB && !status_isdead(bl) && src != bl) {
	  if (damage > 0 )
			mobskill_event((TBL_MOB*)bl,src,gettick(),flag);
	  if (skill_num)
			mobskill_event((TBL_MOB*)bl,src,gettick(),MSC_SKILLUSED|(skill_num<<16));
	}

	return damage;
}

/*==========================================
 * Calculates GVG related damage adjustments.
 *------------------------------------------
 */
int battle_calc_gvg_damage(struct block_list *src,struct block_list *bl,int damage,int div_,int skill_num,int skill_lv,int flag)
{
	struct mob_data *md = NULL;
	int class_;

	if (!damage) //No reductions to make.
		return 0;
	
	class_ = status_get_class(bl);

	if (bl->type == BL_MOB)
		md=(struct mob_data *)bl;
	
	if(md && md->guardian_data) {
		if(class_ == MOBID_EMPERIUM && flag&BF_SKILL)
		//SKill inmunity.
			switch (skill_num) {
			case PA_PRESSURE:
			case MO_TRIPLEATTACK:
			case HW_GRAVITATION:
				break;
			default:
				return 0;
		}
		if(src->type != BL_MOB) {
			struct guild *g=guild_search(status_get_guild_id(src));
			if (!g) return 0;
			if (class_ == MOBID_EMPERIUM && guild_checkskill(g,GD_APPROVAL) <= 0)
				return 0;
			if (battle_config.guild_max_castles &&
				guild_checkcastles(g)>=battle_config.guild_max_castles)
				return 0; // [MouseJstr]
		}
	}

	switch (skill_num) {
	//Skills with no damage reduction.
	case PA_PRESSURE:
	case HW_GRAVITATION:
	case NJ_ZENYNAGE:
		break;
	default:
		if (md && md->guardian_data) {
			damage -= damage
			  	* (md->guardian_data->castle->defense/100)
				* battle_config.castle_defense_rate/100;
		}
		if (flag & BF_SKILL) { //Skills get a different reduction than non-skills. [Skotlex]
			if (flag&BF_WEAPON)
				damage = damage * battle_config.gvg_weapon_damage_rate/100;
			if (flag&BF_MAGIC)
				damage = damage * battle_config.gvg_magic_damage_rate/100;
			if (flag&BF_MISC)
				damage = damage * battle_config.gvg_misc_damage_rate/100;
		} else { //Normal attacks get reductions based on range.
			if (flag & BF_SHORT)
				damage = damage * battle_config.gvg_short_damage_rate/100;
			if (flag & BF_LONG)
				damage = damage * battle_config.gvg_long_damage_rate/100;
		}
		if(!damage) damage  = 1;
	}
	return damage;
}

/*==========================================
 * HP/SPzϋΜvZ
 *------------------------------------------
 */
static int battle_calc_drain(int damage, int rate, int per)
{
	int diff = 0;

	if (per && rand()%1000 < rate) {
		diff = (damage * per) / 100;
		if (diff == 0) {
			if (per > 0)
				diff = 1;
			else
				diff = -1;
		}
	}
	return diff;
}

/*==========================================
 * ?Cϋ_??[W
 *------------------------------------------
 */
int battle_addmastery(struct map_session_data *sd,struct block_list *target,int dmg,int type)
{
	int damage,skill;
	struct status_data *status = status_get_status_data(target);
	int weapon;
	damage = dmg;

	nullpo_retr(0, sd);

	if((skill = pc_checkskill(sd,AL_DEMONBANE)) > 0 &&
		(battle_check_undead(status->race,status->def_ele) || status->race==RC_DEMON) )
		damage += (skill*(int)(3+(sd->status.base_level+1)*0.05));	// submitted by orn
		//damage += (skill * 3);

	if((skill = pc_checkskill(sd,HT_BEASTBANE)) > 0 && (status->race==RC_BRUTE || status->race==RC_INSECT) ) {
		damage += (skill * 4);
		if (sd->sc.data[SC_SPIRIT].timer != -1 && sd->sc.data[SC_SPIRIT].val2 == SL_HUNTER)
			damage += sd->status.str;
	}

	if(type == 0)
		weapon = sd->weapontype1;
	else
		weapon = sd->weapontype2;
	switch(weapon)
	{
		case W_DAGGER:
		case W_1HSWORD:
			if((skill = pc_checkskill(sd,SM_SWORD)) > 0)
				damage += (skill * 4);
			break;
		case W_2HSWORD:
			if((skill = pc_checkskill(sd,SM_TWOHAND)) > 0)
				damage += (skill * 4);
			break;
		case W_1HSPEAR:
		case W_2HSPEAR:
			if((skill = pc_checkskill(sd,KN_SPEARMASTERY)) > 0) {
				if(!pc_isriding(sd))
					damage += (skill * 4);
				else
					damage += (skill * 5);
			}
			break;
		case W_1HAXE:
		case W_2HAXE:
			if((skill = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
				damage += (skill * 3);
			break;
		case W_MACE:
			if((skill = pc_checkskill(sd,PR_MACEMASTERY)) > 0)
				damage += (skill * 3);
			break;
		case W_FIST:
			if((skill = pc_checkskill(sd,TK_RUN)) > 0)
				damage += (skill * 10);
			// No break, fallthrough to Knuckles
		case W_KNUCKLE:
			if((skill = pc_checkskill(sd,MO_IRONHAND)) > 0)
				damage += (skill * 3);
			break;
		case W_MUSICAL:
			if((skill = pc_checkskill(sd,BA_MUSICALLESSON)) > 0)
				damage += (skill * 3);
			break;
		case W_WHIP:
			if((skill = pc_checkskill(sd,DC_DANCINGLESSON)) > 0)
				damage += (skill * 3);
			break;
		case W_BOOK:
			if((skill = pc_checkskill(sd,SA_ADVANCEDBOOK)) > 0)
				damage += (skill * 3);
			break;
		case W_KATAR:
			if((skill = pc_checkskill(sd,AS_KATAR)) > 0)
				damage += (skill * 3);
			break;
	}

	return damage;
}
/*==========================================
 * Calculates the standard damage of a normal attack assuming it hits,
 * it calculates nothing extra fancy, is needed for magnum break's WATK_ELEMENT bonus. [Skotlex]
 *------------------------------------------
 * Pass damage2 as NULL to not calc it.
 * Flag values:
 * &1: Critical hit
 * &2: Arrow attack
 * &4: Skill is Magic Crasher
 * &8: Skip target size adjustment (Extremity Fist?)
 *&16: Arrow attack but BOW, REVOLVER, RIFLE, SHOTGUN, GATLING or GRENADE type weapon not equipped (i.e. shuriken, kunai and venom knives not affected by DEX)
 */
static int battle_calc_base_damage(struct status_data *status, struct weapon_atk *wa, struct status_change *sc, unsigned short t_size, struct map_session_data *sd, int flag)
{
	unsigned short atkmin=0, atkmax=0;
	short type = 0;
	int damage = 0;

	if (!sd)
	{	//Mobs/Pets
		if(flag&4)
		{		  
			atkmin = status->matk_min;
			atkmax = status->matk_max;
		} else {
			atkmin = wa->atk;
			atkmax = wa->atk2;
		}
		if (atkmin > atkmax)
			atkmin = atkmax;
	} else {	//PCs
		atkmax = wa->atk;
		type = (wa == status->lhw)?EQI_HAND_L:EQI_HAND_R;

		if (!(flag&1) || (flag&2))
		{	//Normal attacks
			atkmin = status->dex;
			
			if (sd->equip_index[type] >= 0 && sd->inventory_data[sd->equip_index[type]])
				atkmin = atkmin*(80 + sd->inventory_data[sd->equip_index[type]]->wlv*20)/100;

			if (atkmin > atkmax)
				atkmin = atkmax;
			
			if(flag&2 && !(flag&16))
			{	//Bows
				atkmin = atkmin*atkmax/100;
				if (atkmin > atkmax)
					atkmax = atkmin;
			}
		}
	}
	
	if (sc && sc->data[SC_MAXIMIZEPOWER].timer!=-1)
		atkmin = atkmax;
	
	//Weapon Damage calculation
	if (!(flag&1))
		damage = (atkmax>atkmin? rand()%(atkmax-atkmin):0)+atkmin;
	else 
		damage = atkmax;
	
	if (sd)
	{
		//rodatazone says the range is 0~arrow_atk-1 for non crit
		if (flag&2 && sd->arrow_atk)
			damage += ((flag&1)?sd->arrow_atk:rand()%sd->arrow_atk);

		//SizeFix only for players
		if (!(sd->special_state.no_sizefix || (flag&8)))
			damage = damage*(type==EQI_HAND_L?
				sd->left_weapon.atkmods[t_size]:
				sd->right_weapon.atkmods[t_size])/100;
	}
	
	//Finally, add baseatk
	if(flag&4)
		damage += status->matk_min;
	else
		damage += status->batk;
	
	//rodatazone says that Overrefine bonuses are part of baseatk
	//Here we also apply the weapon_atk_rate bonus so it is correctly applied on left/right hands.
	if(sd) {
		if (type == EQI_HAND_L) {
			if(sd->left_weapon.overrefine)
				damage += rand()%sd->left_weapon.overrefine+1;
			if (sd->weapon_atk_rate[sd->weapontype2])
				damage += damage*sd->weapon_atk_rate[sd->weapontype2]/100;;
		} else { //Right hand
			if(sd->right_weapon.overrefine)
				damage += rand()%sd->right_weapon.overrefine+1;
			if (sd->weapon_atk_rate[sd->weapontype1])
				damage += damage*sd->weapon_atk_rate[sd->weapontype1]/100;;
		}
	}
	return damage;
}

/*==========================================
 * Consumes ammo for the given skill.
 *------------------------------------------
 */
void battle_consume_ammo(TBL_PC*sd, int skill, int lv)
{
	int qty=1;
	if (!battle_config.arrow_decrement)
		return;
	
	if (skill)
	{
		qty = skill_get_ammo_qty(skill, lv);
		if (!qty) qty = 1;
	}

	if(sd->equip_index[EQI_AMMO]>=0) //Qty check should have been done in skill_check_condition
		pc_delitem(sd,sd->equip_index[EQI_AMMO],qty,0);
}

static int battle_range_type(
	struct block_list *src, struct block_list *target,
	int skill_num, int skill_lv)
{	//Skill Range Criteria
	if (battle_config.skillrange_by_distance &&
		(src->type&battle_config.skillrange_by_distance)
	) { //based on distance between src/target [Skotlex]
		if (check_distance_bl(src, target, 5))
			return BF_SHORT;
		return BF_LONG;
	}
	//based on used skill's range
	if (skill_get_range2(src, skill_num, skill_lv) < 5)
		return BF_SHORT;
	return BF_LONG;
}

static int battle_blewcount_bonus(struct map_session_data *sd, int skill_num)
{
	int i;
	if (!sd->skillblown[0].id)
		return 0;
	//Apply the bonus blewcount. [Skotlex]
	for (i = 0; i < MAX_PC_BONUS && sd->skillblown[i].id; i++) {
		if (sd->skillblown[i].id == skill_num)
			return sd->skillblown[i].val;
	}
	return 0;
}

struct Damage battle_calc_magic_attack(struct block_list *src,struct block_list *target,int skill_num,int skill_lv,int mflag);
struct Damage battle_calc_misc_attack(struct block_list *src,struct block_list *target,int skill_num,int skill_lv,int mflag);

//For quick div adjustment.
#define damage_div_fix(dmg, div) { if (div > 1) (dmg)*=div; else if (div < 0) (div)*=-1; }
/*==========================================
 * battle_calc_weapon_attack (by Skotlex)
 *------------------------------------------
 */
static struct Damage battle_calc_weapon_attack(
	struct block_list *src,struct block_list *target,int skill_num,int skill_lv,int wflag)
{
	unsigned short skillratio = 100;	//Skill dmg modifiers.
	short skill=0;
	short s_ele, s_ele_, t_class;
	short i, nk;

	struct map_session_data *sd, *tsd;
	struct Damage wd;
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct {
		unsigned hit : 1; //the attack Hit? (not a miss)
		unsigned cri : 1;		//Critical hit
		unsigned idef : 1;	//Ignore defense
		unsigned idef2 : 1;	//Ignore defense (left weapon)
		unsigned pdef : 2;	//Pierces defense (Investigate/Ice Pick)
		unsigned pdef2 : 2;	//1: Use def+def2/100, 2: Use def+def2/50	
		unsigned infdef : 1;	//Infinite defense (plants)
		unsigned arrow : 1;	//Attack is arrow-based
		unsigned rh : 1;		//Attack considers right hand (wd.damage)
		unsigned lh : 1;		//Attack considers left hand (wd.damage2)
		unsigned weapon : 1; //It's a weapon attack (consider VVS, and all that)
	}	flag;	

	memset(&wd,0,sizeof(wd));
	memset(&flag,0,sizeof(flag));

	if(src==NULL || target==NULL)
	{
		nullpo_info(NLP_MARK);
		return wd;
	}
	//Initial flag
	flag.rh=1;
	flag.weapon=1;
	flag.infdef=(tstatus->mode&MD_PLANT?1:0);

	//Initial Values
	wd.type=0; //Normal attack
	wd.div_=skill_num?skill_get_num(skill_num,skill_lv):1;
	wd.amotion=(skill_num && skill_get_inf(skill_num)&INF_GROUND_SKILL)?0:sstatus->amotion; //Amotion should be 0 for ground skills.
	if(skill_num == KN_AUTOCOUNTER)
		wd.amotion >>= 1;
	wd.dmotion=tstatus->dmotion;
	wd.blewcount=skill_get_blewcount(skill_num,skill_lv);
	wd.flag = BF_WEAPON; //Initial Flag
	wd.flag|= skill_num?BF_SKILL:BF_NORMAL;
	wd.dmg_lv=ATK_DEF;	//This assumption simplifies the assignation later
	nk = skill_get_nk(skill_num);
	flag.hit	= nk&NK_IGNORE_FLEE?1:0;
	flag.idef = flag.idef2 = nk&NK_IGNORE_DEF?1:0;

	if (sc && !sc->count)
		sc = NULL; //Skip checking as there are no status changes active.
	if (tsc && !tsc->count)
		tsc = NULL; //Skip checking as there are no status changes active.

	BL_CAST(BL_PC, src, sd);
	BL_CAST(BL_PC, target, tsd);

	if(sd)
		wd.blewcount += battle_blewcount_bonus(sd, skill_num);

	//Set miscellaneous data that needs be filled regardless of hit/miss
	if(
		(sd && sd->state.arrow_atk) ||
		(!sd && ((skill_num && skill_get_ammotype(skill_num)) || sstatus->rhw.range>3))
	)
		flag.arrow = 1;
	
	if(skill_num){
		wd.flag |= battle_range_type(src, target, skill_num, skill_lv);
		switch(skill_num)
		{
			case MO_FINGEROFFENSIVE:
				if(sd) {
					if (battle_config.finger_offensive_type)
						wd.div_ = 1;
					else
						wd.div_ = sd->spiritball_old;
				}
				break;
			case HT_PHANTASMIC:
				//Since these do not consume ammo, they need to be explicitly set as arrow attacks.
				flag.arrow = 1;
				break;

			case CR_SHIELDBOOMERANG:
			case PA_SHIELDCHAIN:
				flag.weapon = 0;
				break;

			case KN_PIERCE:
				wd.div_= (wd.div_>0?tstatus->size+1:-(tstatus->size+1));
				break;

			case TF_DOUBLE: //For NPC used skill.
			case GS_CHAINACTION:
				wd.type = 0x08;
				break;
				
			case GS_GROUNDDRIFT:
			case KN_SPEARSTAB:
			case KN_BOWLINGBASH:
			case MO_BALKYOUNG:
			case TK_TURNKICK:
				wd.blewcount=0;
				break;

			case KN_AUTOCOUNTER:
				wd.flag=(wd.flag&~BF_SKILLMASK)|BF_NORMAL;
				break;

			case NPC_CRITICALSLASH:
				flag.cri = 1; //Always critical skill.
				break;
		}
	} else //Range for normal attacks.
		wd.flag |= flag.arrow?BF_LONG:BF_SHORT;
	
	if (!skill_num && tstatus->flee2 && rand()%1000 < tstatus->flee2)
	{	//Check for Lucky Dodge
		wd.type=0x0b;
		wd.dmg_lv=ATK_LUCKY;
		if (wd.div_ < 0) wd.div_*=-1;
		return wd;
	}

	t_class = status_get_class(target);
	s_ele = s_ele_ = skill_get_pl(skill_num);
	if (!skill_num || s_ele == -1) { //Take weapon's element
		s_ele = sstatus->rhw.ele;
		s_ele_ = sstatus->lhw?sstatus->lhw->ele:0;
		if (flag.arrow && sd && sd->arrow_ele)
			s_ele = sd->arrow_ele;
	} else if (s_ele == -2) { //Use enchantment's element
		s_ele = s_ele_ = status_get_attack_sc_element(src,sc);
	}
	if (skill_num == GS_GROUNDDRIFT)
		s_ele = s_ele_ = wflag; //element comes in flag.

	if (s_ele == ELE_NEUTRAL && (battle_config.attack_attr_none&src->type))
		nk|=NK_NO_ELEFIX;

	if(!skill_num)
  	{	//Skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
		if (sd && sd->weapontype1 == 0 && sd->weapontype2 > 0)
		{
			flag.rh=0;
			flag.lh=1;
		}
		if (sstatus->lhw && sstatus->lhw->atk)
			flag.lh=1;
	}

	//Check for critical
	if(!flag.cri && sstatus->cri &&
		(!skill_num ||
		skill_num == KN_AUTOCOUNTER ||
		skill_num == SN_SHARPSHOOTING ||
		skill_num == NJ_KIRIKAGE))
	{
		short cri = sstatus->cri;
		if (sd)
		{
			cri+= sd->critaddrace[tstatus->race];
			if(flag.arrow)
				cri += sd->arrow_cri;
			if(sd->status.weapon == W_KATAR)
				cri <<=1;
		}
		//The official equation is *2, but that only applies when sd's do critical.
		//Therefore, we use the old value 3 on cases when an sd gets attacked by a mob
		cri -= tstatus->luk*(!sd&&tsd?3:2);
		
		if(tsc)
		{
			if (tsc->data[SC_SLEEP].timer!=-1 )
				cri <<=1;
		}
		switch (skill_num)
		{
			case KN_AUTOCOUNTER:
				if(battle_config.auto_counter_type &&
					(battle_config.auto_counter_type&src->type))
					flag.cri = 1;
				else
					cri <<= 1;
				break;
			case SN_SHARPSHOOTING:
				cri += 200;
				break;
			case NJ_KIRIKAGE:
				cri += 250 + 50*skill_lv;
				break;
		}
		if(tsd && tsd->critical_def)
			cri = cri*(100-tsd->critical_def)/100;
		if (rand()%1000 < cri)
			flag.cri= 1;
	}
	if (flag.cri)
	{
		wd.type = 0x0a;
		flag.idef = flag.idef2 = flag.hit = 1;
	} else {	//Check for Perfect Hit
		if(sd && sd->perfect_hit > 0 && rand()%100 < sd->perfect_hit)
			flag.hit = 1;
		if (sc && sc->data[SC_FUSION].timer != -1) {
			flag.hit = 1; //SG_FUSION always hit [Komurka]
			flag.idef = flag.idef2 = 1; //def ignore [Komurka]
		}
		if (skill_num && !flag.hit)
			switch(skill_num)
			{
				case AS_SPLASHER: //Reports say it always hits?
					if (wflag) //Only if you were the one exploding.
						break;
					flag.hit = 1;
					break;
				case CR_SHIELDBOOMERANG:
					if (sc && sc->data[SC_SPIRIT].timer != -1 && sc->data[SC_SPIRIT].val2 == SL_CRUSADER)
						flag.hit = 1;
					break;
				case 0:
					//If flag, this is splash damage from Baphomet Card and it always hits.
					if (wflag)
						flag.hit = 1;
					break;
			}
		if (tsc && !flag.hit && tsc->opt1 && tsc->opt1 != OPT1_STONEWAIT)
			flag.hit = 1;
	}

	if (!flag.hit)
	{	//Hit/Flee calculation
		short
			flee = tstatus->flee,
			hitrate=80; //Default hitrate

		if(battle_config.agi_penalty_type &&
			battle_config.agi_penalty_target&target->type)
		{	
			unsigned char target_count; //256 max targets should be a sane max
			target_count = unit_counttargeted(target,battle_config.agi_penalty_count_lv);
			if(target_count >= battle_config.agi_penalty_count)
			{
				if (battle_config.agi_penalty_type == 1)
					flee = (flee * (100 - (target_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num))/100;
				else //asume type 2: absolute reduction
					flee -= (target_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num;
				if(flee < 1) flee = 1;
			}
		}

		hitrate+= sstatus->hit - flee;

		if(wd.flag&BF_LONG && !skill_num && //Fogwall's hit penalty is only for normal ranged attacks.
			tsc && tsc->data[SC_FOGWALL].timer!=-1)
			hitrate-=50;

		if(sd && flag.arrow)
			hitrate += sd->arrow_hit;
		if(skill_num)
			switch(skill_num)
		{	//Hit skill modifiers
			case SM_BASH:
				hitrate += 5*skill_lv;
				break;
			case SM_MAGNUM:
				hitrate += 10*skill_lv;
				break;
			case KN_AUTOCOUNTER:
			case PA_SHIELDCHAIN:
			case NPC_WATERATTACK:
			case NPC_GROUNDATTACK:
			case NPC_FIREATTACK:
			case NPC_WINDATTACK:
			case NPC_POISONATTACK:
			case NPC_HOLYATTACK:
			case NPC_DARKNESSATTACK:
			case NPC_UNDEADATTACK:
			case NPC_TELEKINESISATTACK:
				hitrate += 20;
				break;
			case KN_PIERCE:
				hitrate += hitrate*(5*skill_lv)/100;
				break;
			case AS_SONICBLOW:
				if(sd && pc_checkskill(sd,AS_SONICACCEL)>0)
					hitrate += 50;
				break;
		}

		// Weaponry Research hidden bonus
		if (sd && (skill = pc_checkskill(sd,BS_WEAPONRESEARCH)) > 0)
			hitrate += hitrate*(2*skill)/100;

		if (hitrate > battle_config.max_hitrate)
			hitrate = battle_config.max_hitrate;
		else if (hitrate < battle_config.min_hitrate)
			hitrate = battle_config.min_hitrate;

		if(rand()%100 >= hitrate)
			wd.dmg_lv = ATK_FLEE;
		else
			flag.hit =1;
	}	//End hit/miss calculation

	if (flag.hit && !flag.infdef) //No need to do the math for plants
	{	//Hitting attack

//Assuming that 99% of the cases we will not need to check for the flag.rh... we don't.
//ATK_RATE scales the damage. 100 = no change. 50 is halved, 200 is doubled, etc
#define ATK_RATE( a ) { wd.damage= wd.damage*(a)/100 ; if(flag.lh) wd.damage2= wd.damage2*(a)/100; }
#define ATK_RATE2( a , b ) { wd.damage= wd.damage*(a)/100 ; if(flag.lh) wd.damage2= wd.damage2*(b)/100; }
//Adds dmg%. 100 = +100% (double) damage. 10 = +10% damage
#define ATK_ADDRATE( a ) { wd.damage+= wd.damage*(a)/100 ; if(flag.lh) wd.damage2+= wd.damage2*(a)/100; }
#define ATK_ADDRATE2( a , b ) { wd.damage+= wd.damage*(a)/100 ; if(flag.lh) wd.damage2+= wd.damage2*(b)/100; }
//Adds an absolute value to damage. 100 = +100 damage
#define ATK_ADD( a ) { wd.damage+= a; if (flag.lh) wd.damage2+= a; }
#define ATK_ADD2( a , b ) { wd.damage+= a; if (flag.lh) wd.damage2+= b; }

		switch (skill_num)
		{	//Calc base damage according to skill
			case NJ_ISSEN:
				wd.damage = 40*sstatus->str +skill_lv*(sstatus->hp/10 + 35);
				wd.damage2 = 0;
				status_set_hp(src, 1, 0);
				break;
			case PA_SACRIFICE:
				wd.damage = sstatus->max_hp* 9/100;
				status_zap(src, wd.damage, 0);//Damage to self is always 9%
				wd.damage2 = 0;

				if (sc && sc->data[SC_SACRIFICE].timer != -1)
				{
					if (--sc->data[SC_SACRIFICE].val2 <= 0)
						status_change_end(src, SC_SACRIFICE,-1);
				}
				break;
			case LK_SPIRALPIERCE:
				if (sd) {
					short index = sd->equip_index[EQI_HAND_R];

					if (index >= 0 &&
						sd->inventory_data[index] &&
						sd->inventory_data[index]->type == IT_WEAPON)
						wd.damage = sd->inventory_data[index]->weight*8/100; //80% of weight

					ATK_ADDRATE(50*skill_lv); //Skill modifier applies to weight only.
					index = sstatus->str/10;
					index = index*index;
					ATK_ADD(index); //Add str bonus.

					switch (tstatus->size) { //Size-fix. Is this modified by weapon perfection?
						case 0: //Small: 125%
							ATK_RATE(125);
							break;
						//case 1: //Medium: 100%
						case 2: //Large: 75%
							ATK_RATE(75);
							break;
					}
					break;
				}
			case CR_SHIELDBOOMERANG:
			case PA_SHIELDCHAIN:
				if (sd) {
					short index = sd->equip_index[EQI_HAND_L];

					wd.damage = sstatus->batk;

					if (index >= 0 &&
						sd->inventory_data[index] &&
						sd->inventory_data[index]->type == IT_ARMOR)
						ATK_ADD(sd->inventory_data[index]->weight/10);
					break;
				}
			case HFLI_SBR44:	//[orn]
				if(src->type == BL_HOM) {
					wd.damage = ((TBL_HOM*)src)->homunculus.intimacy ;
					break;
				}
			default:
			{
				i = (flag.cri?1:0)|
					(flag.arrow?2:0)|
					(skill_num == HW_MAGICCRASHER?4:0)|
					(!skill_num && sc && sc->data[SC_CHANGE].timer!=-1?4:0)|
					(skill_num == MO_EXTREMITYFIST?8:0)|
					(sc && sc->data[SC_WEAPONPERFECTION].timer!=-1?8:0);
				if (flag.arrow && sd)
				switch(sd->status.weapon) {
				case W_BOW:
				case W_REVOLVER:
				case W_SHOTGUN:
				case W_GATLING:
				case W_GRENADE:
				  break;
				default:
				  i |= 16; // for ex. shuriken must not be influenced by DEX
				}
				wd.damage = battle_calc_base_damage(sstatus, &sstatus->rhw, sc, tstatus->size, sd, i);
				if (flag.lh)
					wd.damage2 = battle_calc_base_damage(sstatus, sstatus->lhw, sc, tstatus->size, sd, i);

				if (nk&NK_SPLASHSPLIT){ // Divide ATK among targets
					if(wflag>0)
						wd.damage/= wflag;
					else if(battle_config.error_log)
						ShowError("0 enemies targeted by %s, divide per 0 avoided!\n", skill_get_name(skill_num));
				}

				//Add any bonuses that modify the base baseatk+watk (pre-skills)
				if(sd)
				{
					if (sd->atk_rate != 100)
						ATK_RATE(sd->atk_rate);

					if(flag.cri && sd->crit_atk_rate)
						ATK_ADDRATE(sd->crit_atk_rate);

					if(sd->status.party_id && (skill=pc_checkskill(sd,TK_POWER)) > 0){
						i = party_foreachsamemap(party_sub_count, sd, 0);
						ATK_ADDRATE(2*skill*i);
					}
				}
				break;
			}	//End default case
		} //End switch(skill_num)

		//Skill damage modifiers that stack linearly
		if(sc && skill_num != PA_SACRIFICE)
		{
			if(sc->data[SC_OVERTHRUST].timer != -1)
				skillratio += 5*sc->data[SC_OVERTHRUST].val1;
			if(sc->data[SC_MAXOVERTHRUST].timer != -1)
				skillratio += 20*sc->data[SC_MAXOVERTHRUST].val1;
			if(sc->data[SC_BERSERK].timer != -1)
				skillratio += 100;
		}
		if (!skill_num)
		{
			// Random chance to deal multiplied damage - Consider it as part of skill-based-damage
			if(sd &&
				sd->random_attack_increase_add > 0 &&
				sd->random_attack_increase_per &&
				rand()%100 < sd->random_attack_increase_per
				)
				skillratio += sd->random_attack_increase_add;
		
			ATK_RATE(skillratio);
		} else {	//Skills
			switch( skill_num )
			{
				case SM_BASH:
					skillratio += 30*skill_lv;
					break;
				case SM_MAGNUM:
					skillratio += 20*skill_lv; 
					break;
				case MC_MAMMONITE:
					skillratio += 50*skill_lv;
					break;
				case HT_POWER: //FIXME: How exactly is the STR based damage supposed to be done? [Skotlex]
					skillratio += 5*sstatus->str;
					break;
				case AC_DOUBLE:
					skillratio += 10*(skill_lv-1);
					break;
				case AC_SHOWER:
					skillratio += 5*skill_lv-25;
					break;
				case AC_CHARGEARROW:
					skillratio += 50;
					break;
				case HT_FREEZINGTRAP:
					skillratio += -50+10*skill_lv;
					break;
				case KN_PIERCE:
					skillratio += 10*skill_lv;
					break;
				case KN_SPEARSTAB:
					skillratio += 15*skill_lv;
					break;
				case KN_SPEARBOOMERANG:
					skillratio += 50*skill_lv;
					break;
				case KN_BRANDISHSPEAR:
				{
					int ratio = 100+20*skill_lv;
					skillratio += ratio-100;
					if(skill_lv>3 && wflag==1) skillratio += ratio/2;
					if(skill_lv>6 && wflag==1) skillratio += ratio/4;
					if(skill_lv>9 && wflag==1) skillratio += ratio/8;
					if(skill_lv>6 && wflag==2) skillratio += ratio/2;
					if(skill_lv>9 && wflag==2) skillratio += ratio/4;
					if(skill_lv>9 && wflag==3) skillratio += ratio/2;
					break;
				}
				case KN_BOWLINGBASH:
					skillratio+= 40*skill_lv;
					break;
				case AS_GRIMTOOTH:
					skillratio += 20*skill_lv;
					break;
				case AS_POISONREACT:
					skillratio += 30*skill_lv;
					break;
				case AS_SONICBLOW:
					skillratio += -50+5*skill_lv;
					break;
				case TF_SPRINKLESAND:
					skillratio += 30;
					break;
				case MC_CARTREVOLUTION:
					skillratio += 50;
					if(sd && sd->cart_max_weight > 0 && sd->cart_weight > 0)
						skillratio += 100*sd->cart_weight/sd->cart_max_weight; // +1% every 1% weight
					else if (!sd)
						skillratio += 150; //Max damage for non players.
					break;
				case NPC_RANDOMATTACK:
					skillratio += rand()%150-50;
					break;
				case NPC_WATERATTACK:
				case NPC_GROUNDATTACK:
				case NPC_FIREATTACK:
				case NPC_WINDATTACK:
				case NPC_POISONATTACK:
				case NPC_HOLYATTACK:
				case NPC_DARKNESSATTACK:
				case NPC_UNDEADATTACK:
				case NPC_TELEKINESISATTACK:
				case NPC_BLOODDRAIN:
					skillratio += 100*(skill_lv-1);
					break;
				case RG_BACKSTAP:
					if(sd && sd->status.weapon == W_BOW && battle_config.backstab_bow_penalty)
						skillratio += (200+40*skill_lv)/2;
					else
						skillratio += 200+40*skill_lv;
					break;
				case RG_RAID:
					skillratio += 40*skill_lv;
					break;
				case RG_INTIMIDATE:
					skillratio += 30*skill_lv;
					break;
				case CR_SHIELDCHARGE:
					skillratio += 20*skill_lv;
					break;
				case CR_SHIELDBOOMERANG:
					skillratio += 30*skill_lv;
					break;
				case NPC_DARKCROSS:
				case CR_HOLYCROSS:
					skillratio += 35*skill_lv;
					break;
				case AM_DEMONSTRATION:
					skillratio += 20*skill_lv;
					break;
				case AM_ACIDTERROR:
					skillratio += 40*skill_lv;
					break;
				case MO_FINGEROFFENSIVE:
					skillratio+= 50 * skill_lv;
					break;
				case MO_INVESTIGATE:
					skillratio += 75*skill_lv;
					flag.pdef = flag.pdef2 = 2;
					break;
				case MO_EXTREMITYFIST:
					{	//Overflow check. [Skotlex]
						unsigned int ratio = skillratio + 100*(8 + sstatus->sp/10);
						//You'd need something like 6K SP to reach this max, so should be fine for most purposes.
						if (ratio > 60000) ratio = 60000; //We leave some room here in case skillratio gets further increased.
						skillratio = (unsigned short)ratio;
						status_set_sp(src, 0, 0);
					}
					break;
				case MO_TRIPLEATTACK:
					skillratio += 20*skill_lv;
					break;
				case MO_CHAINCOMBO:
					skillratio += 50+50*skill_lv;
					break;
				case MO_COMBOFINISH:
					skillratio += 140+60*skill_lv;
					break;
				case BA_MUSICALSTRIKE:
				case DC_THROWARROW:
					skillratio += 25+25*skill_lv;
					break;
				case CH_TIGERFIST:
					skillratio += 100*skill_lv-60;
					break;
				case CH_CHAINCRUSH:
					skillratio += 300+100*skill_lv;
					break;
				case CH_PALMSTRIKE:
					skillratio += 100+100*skill_lv;
					break;
				case LK_HEADCRUSH:
					skillratio += 40*skill_lv;
					break;
				case LK_JOINTBEAT:
					i = 10*skill_lv-50;
					// Although not clear, it's being assumed that the 2x damage is only for the break neck ailment.
					if (wflag&BREAK_NECK) i*=2;
					skillratio += i;
					break;
				case ASC_METEORASSAULT:
					skillratio += 40*skill_lv-60;
					break;
				case SN_SHARPSHOOTING:
					skillratio += 50*skill_lv;
					break;
				case CG_ARROWVULCAN:
					skillratio += 100+100*skill_lv;
					break;
				case AS_SPLASHER:
					i = 400+50*skill_lv;
					if (sd) i += 20*pc_checkskill(sd,AS_POISONREACT);
					if (wflag>1) i/=wflag; //Splash damage is half.
					skillratio += i;
					break;
				case ASC_BREAKER:
					skillratio += 100*skill_lv-100;
					break;
				case PA_SACRIFICE:
					//40% less effective on siege maps. [Skotlex]
					skillratio += 10*skill_lv-10;
					break;
				case PA_SHIELDCHAIN:
					skillratio += 30*skill_lv;
					break;
				case WS_CARTTERMINATION:
					i = 10 * (16 - skill_lv);
					if (i < 1) i = 1;
					//Preserve damage ratio when max cart weight is changed.
					if(sd && sd->cart_weight && sd->cart_max_weight)
						skillratio += sd->cart_weight/i * 80000/sd->cart_max_weight - 100;
					else if (!sd)
						skillratio += 80000 / i - 100;
					break;
				case TK_DOWNKICK:
					skillratio += 60 + 20*skill_lv;
					break;
				case TK_STORMKICK:
					skillratio += 60 + 20*skill_lv;
					break;
				case TK_TURNKICK:
					skillratio += 90 + 30*skill_lv;
					break;
				case TK_COUNTER:
					skillratio += 90 + 30*skill_lv;
					break;
				case TK_JUMPKICK:
					skillratio += -70 + 10*skill_lv;
					if (sc && sc->data[SC_COMBO].timer != -1 && sc->data[SC_COMBO].val1 == skill_num)
						skillratio += 10*status_get_lv(src)/3;
					break;
				case GS_TRIPLEACTION:
					skillratio += 50*skill_lv;
					break;
				case GS_BULLSEYE:
					//Only works well against brute/demihumans non bosses.
					if((tstatus->race == RC_BRUTE || tstatus->race == RC_DEMIHUMAN)
						&& !(tstatus->mode&MD_BOSS))
						skillratio += 400;
					break;
				case GS_TRACKING:
					skillratio += 100 *(skill_lv+1);
					break;
				case GS_PIERCINGSHOT:
					skillratio += 20*skill_lv;
					break;
				case GS_RAPIDSHOWER:
					skillratio += 10*skill_lv;
					break;
				case GS_DESPERADO:
					skillratio += 50*(skill_lv-1);
					break;
				case GS_DUST:
					skillratio += 50*skill_lv;
					break;
				case GS_FULLBUSTER:
					skillratio += 100*(skill_lv+2);
					break;
				case GS_SPREADATTACK:
					skillratio += 20*(skill_lv-1);
					break;
				case NJ_HUUMA:
					skillratio += 50 + 150*skill_lv;
					break;
				case NJ_TATAMIGAESHI:
					skillratio += 10*skill_lv;
					break;
				case NJ_KASUMIKIRI:
					skillratio += 10*skill_lv;
					break;
				case NJ_KIRIKAGE:
					skillratio += 100*(skill_lv-1);
					break;
				case KN_CHARGEATK:
					skillratio += 100*((wflag-1)/3); //+100% every 3 cells.of distance
					break;
				case HT_PHANTASMIC:
					skillratio += 50;
					break;
				case MO_BALKYOUNG:
					skillratio += 200;
					break;
				case HFLI_MOON:	//[orn]
					skillratio += 10+110*skill_lv;
					break;
				case HFLI_SBR44:	//[orn]
					skillratio += 100 *(skill_lv-1);
					break;
			}

			ATK_RATE(skillratio);

			//Constant/misc additions from skills
			switch (skill_num) {
				case MO_EXTREMITYFIST:
					ATK_ADD(250 + 150*skill_lv);
					break;
				case TK_DOWNKICK:
				case TK_STORMKICK:
				case TK_TURNKICK:
				case TK_COUNTER:
				case TK_JUMPKICK:
					//TK_RUN kick damage bonus.
					if(sd && sd->weapontype1 == W_FIST && sd->weapontype2 == W_FIST)
						ATK_ADD(10*pc_checkskill(sd, TK_RUN));
					break;
				case GS_MAGICALBULLET:
					if(sstatus->matk_max>sstatus->matk_min) {
						ATK_ADD(sstatus->matk_min+rand()%(sstatus->matk_max-sstatus->matk_min));
					} else {
						ATK_ADD(sstatus->matk_min);
					}
					break;
				case NJ_SYURIKEN:
					ATK_ADD(4*skill_lv);
					break;
			}
		}
		//Div fix.
		damage_div_fix(wd.damage, wd.div_);

		//The following are applied on top of current damage and are stackable.
		if (sc) {
			if(sc->data[SC_TRUESIGHT].timer != -1)
				ATK_ADDRATE(2*sc->data[SC_TRUESIGHT].val1);

			if(sc->data[SC_EDP].timer != -1 &&
			  	skill_num != ASC_BREAKER &&
				skill_num != ASC_METEORASSAULT &&
				skill_num != AS_VENOMKNIFE)
				ATK_ADDRATE(sc->data[SC_EDP].val3);
		}

		switch (skill_num) {
			case AS_SONICBLOW:
				if (sc && sc->data[SC_SPIRIT].timer != -1 &&
					sc->data[SC_SPIRIT].val2 == SL_ASSASIN)
					ATK_ADDRATE(map_flag_gvg(src->m)?25:100); //+25% dmg on woe/+100% dmg on nonwoe

				if(sd && pc_checkskill(sd,AS_SONICACCEL)>0)
					ATK_ADDRATE(10);
			break;
			case CR_SHIELDBOOMERANG:
				if(sc && sc->data[SC_SPIRIT].timer != -1 &&
					sc->data[SC_SPIRIT].val2 == SL_CRUSADER)
					ATK_ADDRATE(100);
				break;
		}
		
		if(sd)
		{
			if (skill_num && sd->skillatk[0].id)
			{	//Additional skill damage.
				for (i = 0; i < MAX_PC_BONUS && sd->skillatk[i].id &&
				  	sd->skillatk[i].id != skill_num; i++);

				if (i < MAX_PC_BONUS && sd->skillatk[i].id == skill_num)
					ATK_ADDRATE(sd->skillatk[i].val);
			}

			if(skill_num != PA_SACRIFICE && skill_num != MO_INVESTIGATE &&
				skill_num != CR_GRANDCROSS && skill_num != NPC_GRANDDARKNESS &&
				skill_num != PA_SHIELDCHAIN
			  	&& !flag.cri)
			{	//Elemental/Racial adjustments
				if(sd->right_weapon.def_ratio_atk_ele & (1<<tstatus->def_ele) ||
					sd->right_weapon.def_ratio_atk_race & (1<<tstatus->race) ||
					sd->right_weapon.def_ratio_atk_race & (1<<(is_boss(target)?RC_BOSS:RC_NONBOSS))
				)
					flag.pdef = 1;

				if(sd->left_weapon.def_ratio_atk_ele & (1<<tstatus->def_ele) ||
					sd->left_weapon.def_ratio_atk_race & (1<<tstatus->race) ||
					sd->left_weapon.def_ratio_atk_race & (1<<(is_boss(target)?RC_BOSS:RC_NONBOSS))
				) {	//Pass effect onto right hand if configured so. [Skotlex]
					if (battle_config.left_cardfix_to_right && flag.rh)
						flag.pdef = 1;
					else
						flag.pdef2 = 1;
				}
			}

			if (skill_num != CR_GRANDCROSS && skill_num != NPC_GRANDDARKNESS)
		  	{	//Ignore Defense?
				if (!flag.idef && (
					(target->type == BL_MOB && sd->right_weapon.ignore_def_mob & (is_boss(target)?2:1)) ||
					sd->right_weapon.ignore_def_ele & (1<<tstatus->def_ele) ||
					sd->right_weapon.ignore_def_race & (1<<tstatus->race) ||
					sd->right_weapon.ignore_def_race & (is_boss(target)?1<<RC_BOSS:1<<RC_NONBOSS)
				))
					flag.idef = 1;

				if (!flag.idef2 && (
					(target->type == BL_MOB && sd->left_weapon.ignore_def_mob & (is_boss(target)?2:1)) ||
					sd->left_weapon.ignore_def_ele & (1<<tstatus->def_ele) ||
					sd->left_weapon.ignore_def_race & (1<<tstatus->race) ||
					sd->left_weapon.ignore_def_race & (is_boss(target)?1<<RC_BOSS:1<<RC_NONBOSS)
				)) {
						if(battle_config.left_cardfix_to_right && flag.rh) //Move effect to right hand. [Skotlex]
							flag.idef = 1;
						else
							flag.idef2 = 1;
				}
			}
		}

		if (!flag.idef || !flag.idef2)
		{	//Defense reduction
			short vit_def;
			signed char def1 = (signed char)status_get_def(target); //Don't use tstatus->def1 due to skill timer reductions.
			short def2 = (short)tstatus->def2;
			if(battle_config.vit_penalty_type &&
				battle_config.vit_penalty_target&target->type)
			{
				unsigned char target_count; //256 max targets should be a sane max
				target_count = unit_counttargeted(target,battle_config.vit_penalty_count_lv);
				if(target_count >= battle_config.vit_penalty_count) {
					if(battle_config.vit_penalty_type == 1) {
						def1 = (def1 * (100 - (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num))/100;
						def2 = (def2 * (100 - (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num))/100;
					} else { //Assume type 2
						def1 -= (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num;
						def2 -= (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num;
					}
				}
				if(def1 < 0 || skill_num == AM_ACIDTERROR) def1 = 0; //Acid Terror ignores only armor defense. [Skotlex]
				if(def2 < 1) def2 = 1;
			}
			//Vitality reduction from rodatazone: http://rodatazone.simgaming.net/mechanics/substats.php#def	
			if (tsd)	//Sd vit-eq
			{	//[VIT*0.5] + rnd([VIT*0.3], max([VIT*0.3],[VIT^2/150]-1))
				vit_def = def2*(def2-15)/150;
				vit_def = def2/2 + (vit_def>0?rand()%vit_def:0);
				
				if((battle_check_undead(sstatus->race,sstatus->def_ele) || sstatus->race==RC_DEMON) &&
					(skill=pc_checkskill(tsd,AL_DP)) >0)
					vit_def += skill*(int)(3 +(tsd->status.base_level+1)*0.04);   // submitted by orn
			} else { //Mob-Pet vit-eq
				//VIT + rnd(0,[VIT/20]^2-1)
				vit_def = (def2/20)*(def2/20);
				vit_def = def2 + (vit_def>0?rand()%vit_def:0);
			}
			
			if (battle_config.weapon_defense_type) {
				vit_def += def1*battle_config.weapon_defense_type;
				def1 = 0;
			}
			if (def1 > 100) def1 = 100;
			ATK_RATE2(
				flag.idef ?100:
				(flag.pdef ?flag.pdef *(def1 + vit_def):
				100-def1),
			  	flag.idef2?100:
				(flag.pdef2?flag.pdef2*(def1 + vit_def):
				100-def1)
			);
			ATK_ADD2(
				flag.idef ||flag.pdef ?0:-vit_def,
				flag.idef2||flag.pdef2?0:-vit_def
			);
		}

		//Post skill/vit reduction damage increases
		if (sc && skill_num != LK_SPIRALPIERCE)
		{	//SC skill damages
			if(sc->data[SC_AURABLADE].timer!=-1) 
				ATK_ADD(20*sc->data[SC_AURABLADE].val1);
		}

		//Refine bonus
		if (sd && flag.weapon && skill_num != MO_INVESTIGATE && skill_num != MO_EXTREMITYFIST) {
			if (skill_num == MO_FINGEROFFENSIVE) //Counts refine bonus multiple times
			{
				ATK_ADD2(wd.div_*sstatus->rhw.atk2, wd.div_*sstatus->lhw->atk2);
			} else {
				ATK_ADD2(sstatus->rhw.atk2, sstatus->lhw->atk2);
			}
		}

		//Set to min of 1
		if (flag.rh && wd.damage < 1) wd.damage = 1;
		if (flag.lh && wd.damage2 < 1) wd.damage2 = 1;

		if (sd && flag.weapon &&
			skill_num != MO_INVESTIGATE &&
		  	skill_num != MO_EXTREMITYFIST &&
		  	skill_num != CR_GRANDCROSS)
		{	//Add mastery damage
			if(skill_num != ASC_BREAKER && sd->status.weapon == W_KATAR &&
				(skill=pc_checkskill(sd,ASC_KATAR)) > 0)
		  	{	//Adv Katar Mastery is does not applies to ASC_BREAKER,
				// but other masteries DO apply >_>
				ATK_ADDRATE(10+ 2*skill);
			}

			wd.damage = battle_addmastery(sd,target,wd.damage,0);
			if (flag.lh)
				wd.damage2 = battle_addmastery(sd,target,wd.damage2,1);

			if((skill=pc_checkskill(sd,SG_STAR_ANGER)) >0 && (t_class == sd->hate_mob[2] ||
				(sc && sc->data[SC_MIRACLE].timer!=-1)))
			{
				skillratio = sd->status.base_level + sstatus->str + sstatus->dex + sstatus->luk;
				if (skill<4)
					skillratio /= 12-3*skill;
				ATK_ADDRATE(skillratio);
			} else
			if(
				((skill=pc_checkskill(sd,SG_SUN_ANGER)) >0 && t_class == sd->hate_mob[0]) ||
				((skill=pc_checkskill(sd,SG_MOON_ANGER)) >0 && t_class == sd->hate_mob[1])
			) {
				skillratio = sd->status.base_level + sstatus->dex+ sstatus->luk;
				if (skill<4)
					skillratio /= 12-3*skill;
				ATK_ADDRATE(skillratio);
			}
			if (skill_num == NJ_SYURIKEN && (skill = pc_checkskill(sd,NJ_TOBIDOUGU)) > 0)
				ATK_ADD(3*skill);
			if (skill_num == NJ_KUNAI)
				ATK_ADD(60);
		}
	} //Here ends flag.hit section, the rest of the function applies to both hitting and missing attacks
  	else if(wd.div_ < 0) //Since the attack missed...
		wd.div_ *= -1; 

	if(skill_num == CR_GRANDCROSS || skill_num == NPC_GRANDDARKNESS)
		return wd; //Enough, rest is not needed.

	if(sd && (skill=pc_checkskill(sd,BS_WEAPONRESEARCH)) > 0) 
		ATK_ADD(skill*2);

	if(skill_num==TF_POISON)
		ATK_ADD(15*skill_lv);

	if (!(nk&NK_NO_ELEFIX))
	{	//Elemental attribute fix
		if (wd.damage > 0)
		{
			wd.damage=battle_attr_fix(src,target,wd.damage,s_ele,tstatus->def_ele, tstatus->ele_lv);
			if(skill_num==MC_CARTREVOLUTION) //Cart Revolution applies the element fix once more with neutral element
				wd.damage = battle_attr_fix(src,target,wd.damage,ELE_NEUTRAL,tstatus->def_ele, tstatus->ele_lv);
			if(skill_num== GS_GROUNDDRIFT) //Additional 50*lv Neutral damage.
				wd.damage+= battle_attr_fix(src,target,50*skill_lv,ELE_NEUTRAL,tstatus->def_ele, tstatus->ele_lv);
		}
		if (flag.lh && wd.damage2 > 0)
			wd.damage2 = battle_attr_fix(src,target,wd.damage2,s_ele_,tstatus->def_ele, tstatus->ele_lv);
		if(sc && sc->data[SC_WATK_ELEMENT].timer != -1)
		{	//Descriptions indicate this means adding a percent of a normal attack in another element. [Skotlex]
			int damage= battle_calc_base_damage(sstatus, &sstatus->rhw, sc, tstatus->size, sd, (flag.arrow?2:0));
			damage = damage*sc->data[SC_WATK_ELEMENT].val2/100;
			damage = battle_attr_fix(src,target,damage,sc->data[SC_WATK_ELEMENT].val1,tstatus->def_ele, tstatus->ele_lv);
			ATK_ADD(damage);
		}
	}

	if (sd)
	{
		if (skill_num != CR_SHIELDBOOMERANG) //Only Shield boomerang doesn't takes the Star Crumbs bonus.
			ATK_ADD2(wd.div_*sd->right_weapon.star, wd.div_*sd->left_weapon.star);
		if (skill_num==MO_FINGEROFFENSIVE) { //The finger offensive spheres on moment of attack do count. [Skotlex]
			ATK_ADD(wd.div_*sd->spiritball_old*3);
		} else {
			ATK_ADD(wd.div_*sd->spiritball*3);
		}

		//Card Fix, sd side
		if ((wd.damage || wd.damage2) && !(nk&NK_NO_CARDFIX_ATK))
		{
			short cardfix = 1000, cardfix_ = 1000;
			short t_race2 = status_get_race2(target);	
			if(sd->state.arrow_atk)
			{
				cardfix=cardfix*(100+sd->right_weapon.addrace[tstatus->race]+sd->arrow_addrace[tstatus->race])/100;
				if (!(nk&NK_NO_ELEFIX))
					cardfix=cardfix*(100+sd->right_weapon.addele[tstatus->def_ele]+sd->arrow_addele[tstatus->def_ele])/100;
				cardfix=cardfix*(100+sd->right_weapon.addsize[tstatus->size]+sd->arrow_addsize[tstatus->size])/100;
				cardfix=cardfix*(100+sd->right_weapon.addrace2[t_race2])/100;
				cardfix=cardfix*(100+sd->right_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS]+sd->arrow_addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
			} else {	//Melee attack
				if(!battle_config.left_cardfix_to_right)
				{
					cardfix=cardfix*(100+sd->right_weapon.addrace[tstatus->race])/100;
					if (!(nk&NK_NO_ELEFIX))
						cardfix=cardfix*(100+sd->right_weapon.addele[tstatus->def_ele])/100;
					cardfix=cardfix*(100+sd->right_weapon.addsize[tstatus->size])/100;
					cardfix=cardfix*(100+sd->right_weapon.addrace2[t_race2])/100;
					cardfix=cardfix*(100+sd->right_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;

					if (flag.lh)
					{
						cardfix_=cardfix_*(100+sd->left_weapon.addrace[tstatus->race])/100;
						if (!(nk&NK_NO_ELEFIX))
							cardfix_=cardfix_*(100+sd->left_weapon.addele[tstatus->def_ele])/100;
						cardfix_=cardfix_*(100+sd->left_weapon.addsize[tstatus->size])/100;
						cardfix_=cardfix_*(100+sd->left_weapon.addrace2[t_race2])/100;
						cardfix_=cardfix_*(100+sd->left_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
					}
				} else {
					cardfix=cardfix*(100+sd->right_weapon.addrace[tstatus->race]+sd->left_weapon.addrace[tstatus->race])/100;
					cardfix=cardfix*(100+sd->right_weapon.addele[tstatus->def_ele]+sd->left_weapon.addele[tstatus->def_ele])/100;
					cardfix=cardfix*(100+sd->right_weapon.addsize[tstatus->size]+sd->left_weapon.addsize[tstatus->size])/100;
					cardfix=cardfix*(100+sd->right_weapon.addrace2[t_race2]+sd->left_weapon.addrace2[t_race2])/100;
					cardfix=cardfix*(100+sd->right_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS]+sd->left_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
				}
			}

			for(i=0;i<sd->right_weapon.add_damage_class_count;i++) {
				if(sd->right_weapon.add_damage_classid[i] == t_class) {
					cardfix=cardfix*(100+sd->right_weapon.add_damage_classrate[i])/100;
					break;
				}
			}

			if (flag.lh)
			{
				for(i=0;i<sd->left_weapon.add_damage_class_count;i++) {
					if(sd->left_weapon.add_damage_classid[i] == t_class) {
						cardfix_=cardfix_*(100+sd->left_weapon.add_damage_classrate[i])/100;
						break;
					}
				}
			}

			if(wd.flag&BF_LONG)
				cardfix=cardfix*(100+sd->long_attack_atk_rate)/100;

			if (cardfix != 1000 || cardfix_ != 1000)
				ATK_RATE2(cardfix/10, cardfix_/10);	//What happens if you use right-to-left and there's no right weapon, only left?
		}
		
		if (skill_num == CR_SHIELDBOOMERANG || skill_num == PA_SHIELDCHAIN) { //Refine bonus applies after cards and elements.
			short index= sd->equip_index[EQI_HAND_L];
			if (index >= 0 &&
				sd->inventory_data[index] &&
				sd->inventory_data[index]->type == IT_ARMOR)
				ATK_ADD(10*sd->status.inventory[index].refine);
		}
	} //if (sd)

	//Card Fix, tsd sid
	if (tsd && !(nk&NK_NO_CARDFIX_DEF))
	{
		short s_race2,s_class;
		short cardfix=1000;

		s_race2 = status_get_race2(src);
		s_class = status_get_class(src);

		if (!(nk&NK_NO_ELEFIX))
		{
			cardfix=cardfix*(100-tsd->subele[s_ele])/100;
			if (flag.lh && s_ele_ != s_ele)
				cardfix=cardfix*(100-tsd->subele[s_ele_])/100;
		}
		cardfix=cardfix*(100-tsd->subsize[sstatus->size])/100;
 		cardfix=cardfix*(100-tsd->subrace2[s_race2])/100;
		cardfix=cardfix*(100-tsd->subrace[sstatus->race])/100;
		cardfix=cardfix*(100-tsd->subrace[is_boss(src)?RC_BOSS:RC_NONBOSS])/100;

		for(i=0;i<tsd->add_dmg_count;i++) {
			if(tsd->add_dmg[i].class_ == s_class) {
				cardfix=cardfix*(100+tsd->add_dmg[i].rate)/100;
				break;
			}
		}

		if(wd.flag&BF_SHORT)
			cardfix=cardfix*(100-tsd->near_attack_def_rate)/100;
		else	// BF_LONG (there's no other choice)
			cardfix=cardfix*(100-tsd->long_attack_def_rate)/100;

		if (cardfix != 1000)
			ATK_RATE(cardfix/10);
	}

	if(flag.infdef)
	{ //Plants receive 1 damage when hit
		if (flag.rh && (flag.hit || wd.damage>0))
			wd.damage = 1;
		if (flag.lh && (flag.hit || wd.damage2>0))
			wd.damage2 = 1;
		if (!(battle_config.skill_min_damage&1)) 
			//Do not return if you are supposed to deal greater damage to plants than 1. [Skotlex]
			return wd;
	}

	if(sd && !skill_num && !flag.cri)
	{	//Check for double attack.
		if(((skill_lv = pc_checkskill(sd,TF_DOUBLE)) > 0 && sd->weapontype1 == W_DAGGER) || sd->double_rate > 0)
		{	//Success chance is not added, the higher one is used [Skotlex]
			if (rand()%100 < (5*skill_lv>sd->double_rate?5*skill_lv:sd->double_rate))
			{
				wd.div_=skill_get_num(TF_DOUBLE,skill_lv?skill_lv:1);
				damage_div_fix(wd.damage, wd.div_);
				wd.type = 0x08;
			}
		} else
	  	if (sd->weapontype1 == W_REVOLVER &&
			(skill_lv = pc_checkskill(sd,GS_CHAINACTION)) > 0 &&
			(rand()%100 < 5*skill_lv)
			)
		{
			wd.div_=skill_get_num(GS_CHAINACTION,skill_lv);
			damage_div_fix(wd.damage, wd.div_);
			wd.type = 0x08;
		}
	}

	if (sd)
	{
		if (!flag.rh && flag.lh) 
		{	//Move lh damage to the rh
			wd.damage = wd.damage2;
			wd.damage2 = 0;
			flag.rh=1;
			flag.lh=0;
		} else if(flag.rh && flag.lh)
		{	//Dual-wield
			if (wd.damage)
			{
				skill = pc_checkskill(sd,AS_RIGHT);
				wd.damage = wd.damage * (50 + (skill * 10))/100;
				if(wd.damage < 1) wd.damage = 1;
			}
			if (wd.damage2)
			{
				skill = pc_checkskill(sd,AS_LEFT);
				wd.damage2 = wd.damage2 * (30 + (skill * 10))/100;
				if(wd.damage2 < 1) wd.damage2 = 1;
			}
		} else if(sd->status.weapon == W_KATAR && !skill_num)
		{ //Katars (offhand damage only applies to normal attacks, tested on Aegis 10.2)
			skill = pc_checkskill(sd,TF_DOUBLE);
			wd.damage2 = wd.damage * (1 + (skill * 2))/100;

			if(wd.damage && !wd.damage2) wd.damage2 = 1;
			flag.lh = 1;
		}
	}

	if(!flag.rh && wd.damage)
		wd.damage=0;

	if(!flag.lh && wd.damage2)
		wd.damage2=0;

	if(wd.damage + wd.damage2)
	{	//There is a total damage value
		if(!wd.damage2) {
			wd.damage=battle_calc_damage(src,target,wd.damage,wd.div_,skill_num,skill_lv,wd.flag);
			if (map_flag_gvg2(target->m))
				wd.damage=battle_calc_gvg_damage(src,target,wd.damage,wd.div_,skill_num,skill_lv,wd.flag);
		} else
		if(!wd.damage) {
			wd.damage2=battle_calc_damage(src,target,wd.damage2,wd.div_,skill_num,skill_lv,wd.flag);
			if (map_flag_gvg2(target->m))
				wd.damage2=battle_calc_gvg_damage(src,target,wd.damage2,wd.div_,skill_num,skill_lv,wd.flag);
		} else
		{
			int d1=wd.damage+wd.damage2,d2=wd.damage2;
			wd.damage=battle_calc_damage(src,target,d1,wd.div_,skill_num,skill_lv,wd.flag);
			if (map_flag_gvg2(target->m))
				wd.damage=battle_calc_gvg_damage(src,target,wd.damage,wd.div_,skill_num,skill_lv,wd.flag);
			wd.damage2=(d2*100/d1)*wd.damage/100;
			if(wd.damage > 1 && wd.damage2 < 1) wd.damage2=1;
			wd.damage-=wd.damage2;
		}
	}

	if(skill_num==ASC_BREAKER)
	{	//Breaker's int-based damage (a misc attack?)
		struct Damage md = battle_calc_misc_attack(src, target, skill_num, skill_lv, wflag);
		wd.damage += md.damage;
	}

	if (wd.damage || wd.damage2) {
		if (sd && battle_config.equip_self_break_rate)
		{	// Self weapon breaking
			int breakrate = battle_config.equip_natural_break_rate;
			if (sc) {
				if(sc->data[SC_OVERTHRUST].timer!=-1)
					breakrate += 10;
				if(sc->data[SC_MAXOVERTHRUST].timer!=-1)
					breakrate += 10;
			}
			if (breakrate)
				skill_break_equip(src, EQP_WEAPON, breakrate, BCT_SELF);
		}
		//Cart Termination won't trigger breaking data. Why? No idea, go ask Gravity.
		if (battle_config.equip_skill_break_rate && skill_num != WS_CARTTERMINATION)
		{	// Target equipment breaking
			int breakrate[2] = {0,0}; // weapon = 0, armor = 1
			if (sd) {	// Break rate from equipment
				breakrate[0] += sd->break_weapon_rate;
				breakrate[1] += sd->break_armor_rate;
			}
			if (sc) {
				if (sc->data[SC_MELTDOWN].timer!=-1) {
					breakrate[0] += sc->data[SC_MELTDOWN].val2;
					breakrate[1] += sc->data[SC_MELTDOWN].val3;
				}
			}	
			if (breakrate[0])
				skill_break_equip(target, EQP_WEAPON, breakrate[0], BCT_ENEMY);
			if (breakrate[1])
				skill_break_equip(target, EQP_ARMOR, breakrate[1], BCT_ENEMY);
		}
	}

	//SG_FUSION hp penalty [Komurka]
	if (sc && sc->data[SC_FUSION].timer!=-1)
	{
		int hp= sstatus->max_hp;
		if (sd && tsd) {
			hp = 8*hp/100;
			if (100*sstatus->hp <= 20*sstatus->max_hp)
				hp = sstatus->hp;
		} else
			hp = 5*hp/1000;
		status_zap(src, hp, 0);
	}

	return wd;
}

/*==========================================
 * battle_calc_magic_attack [DracoRPG]
 *------------------------------------------
 */
struct Damage battle_calc_magic_attack(
	struct block_list *src,struct block_list *target,int skill_num,int skill_lv,int mflag)
	{
	short i, nk;
	short s_ele;
	unsigned short skillratio = 100;	//Skill dmg modifiers.

	struct map_session_data *sd, *tsd;
	struct Damage ad;
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct {
		unsigned imdef : 1;
		unsigned infdef : 1;
	}	flag;

	memset(&ad,0,sizeof(ad));
	memset(&flag,0,sizeof(flag));

	if(src==NULL || target==NULL)
	{
		nullpo_info(NLP_MARK);
		return ad;
	}
	//Initial Values
	ad.damage = 1;
	ad.div_=skill_get_num(skill_num,skill_lv);
	ad.amotion=skill_get_inf(skill_num)&INF_GROUND_SKILL?0:sstatus->amotion; //Amotion should be 0 for ground skills.
	ad.dmotion=tstatus->dmotion;
	ad.blewcount = skill_get_blewcount(skill_num,skill_lv);
	ad.flag=BF_MAGIC|BF_SKILL;
	ad.dmg_lv=ATK_DEF;
	nk = skill_get_nk(skill_num);
	flag.imdef = nk&NK_IGNORE_DEF?1:0;

	BL_CAST(BL_PC, src, sd);
	BL_CAST(BL_PC, target, tsd);

	//Initialize variables that will be used afterwards
	s_ele = skill_get_pl(skill_num);

	if (s_ele == -1) // pl=-1 : the skill takes the weapon's element
		s_ele = sstatus->rhw.ele;
	else if (s_ele == -2) //Use status element
		s_ele = status_get_attack_sc_element(src,status_get_sc(src));
	
	//Set miscellaneous data that needs be filled
	if(sd) {
		sd->state.arrow_atk = 0;
		ad.blewcount += battle_blewcount_bonus(sd, skill_num);
	}

	//Skill Range Criteria
	ad.flag |= battle_range_type(src, target, skill_num, skill_lv);
	flag.infdef=(tstatus->mode&MD_PLANT?1:0);
		
	switch(skill_num)
	{
		case MG_FIREWALL:
			if(mflag) //mflag has a value when it was checked against an undead in skill.c [Skotlex]
				ad.blewcount = 0; //No knockback
			else
				ad.blewcount |= 0x10000;
			ad.dmotion = 0; //No flinch animation.
			break;
		case WZ_STORMGUST: //Should knockback randomly.
			ad.blewcount|=0x40000;
			break;
		case PR_SANCTUARY:
			ad.blewcount|=0x10000;
			ad.dmotion = 0; //No flinch animation.
			break;
	}

	if (!flag.infdef) //No need to do the math for plants
	{

//MATK_RATE scales the damage. 100 = no change. 50 is halved, 200 is doubled, etc
#define MATK_RATE( a ) { ad.damage= ad.damage*(a)/100; }
//Adds dmg%. 100 = +100% (double) damage. 10 = +10% damage
#define MATK_ADDRATE( a ) { ad.damage+= ad.damage*(a)/100; }
//Adds an absolute value to damage. 100 = +100 damage
#define MATK_ADD( a ) { ad.damage+= a; }

		switch (skill_num)
		{	//Calc base damage according to skill
			case AL_HEAL:
			case PR_BENEDICTIO:
				ad.damage = skill_calc_heal(src,skill_lv)/2;
				break;
			case PR_ASPERSIO:
				ad.damage = 40;
				break;
			case PR_SANCTUARY:
				ad.damage = (skill_lv>6)?388:skill_lv*50;
				break;
			case ALL_RESURRECTION:
			case PR_TURNUNDEAD:
				//Undead check is on skill_castend_damageid code.
				i = 20*skill_lv + sstatus->luk + sstatus->int_ + status_get_lv(src)
				  	+ 200 - 200*tstatus->hp/tstatus->max_hp;
				if(i > 700) i = 700;
				if(rand()%1000 < i && !(tstatus->mode&MD_BOSS))
					ad.damage = tstatus->hp;
				else
					ad.damage = status_get_lv(src) + sstatus->int_ + skill_lv * 10;
				break;
			case PF_SOULBURN:
				ad.damage = tstatus->sp * 2;
				break;
			case HW_GRAVITATION:
				ad.damage = 200+200*skill_lv;
				break;
			default:
			{
				if (sstatus->matk_max > sstatus->matk_min) {
					MATK_ADD(sstatus->matk_min+rand()%(1+sstatus->matk_max-sstatus->matk_min));
				} else {
					MATK_ADD(sstatus->matk_min);
				}

				if(nk&NK_SPLASHSPLIT){ // Divide MATK in case of multiple targets skill
					if(mflag>0)
						ad.damage/= mflag;
					else if(battle_config.error_log)
						ShowError("0 enemies targeted by %s, divide per 0 avoided!\n", skill_get_name(skill_num));
				}

				switch(skill_num){
					case MG_NAPALMBEAT:
					case MG_FIREBALL:
						skillratio += skill_lv*10-30;
						break;
					case MG_SOULSTRIKE:
						if (battle_check_undead(tstatus->race,tstatus->def_ele))
							skillratio += 5*skill_lv;
						break;
					case MG_FIREWALL:
						skillratio -= 50;
						break;
					case MG_THUNDERSTORM:
						skillratio -= 20;
						break;
					case MG_FROSTDIVER:
						skillratio += 10*skill_lv;
						break;
					case AL_HOLYLIGHT:
						skillratio += 25;
						if (sd && sd->sc.data[SC_SPIRIT].timer != -1 && sd->sc.data[SC_SPIRIT].val2 == SL_PRIEST)
							skillratio *= 5; //Does 5x damage include bonuses from other skills?
						break;
					case AL_RUWACH:
						skillratio += 45;
						break;
					case WZ_FROSTNOVA:
						skillratio += (100+skill_lv*10)*2/3-100;
						break;
					case WZ_FIREPILLAR:
						if (skill_lv > 10)
							skillratio += 100;
						else
							skillratio -= 80;
						break;
					case WZ_SIGHTRASHER:
						skillratio += 20*skill_lv;
						break;
					case WZ_VERMILION:
						skillratio += 20*skill_lv-20;
						break;
					case WZ_WATERBALL:
						skillratio += 30*skill_lv;
						break;
					case WZ_STORMGUST:
						skillratio += 40*skill_lv;
						break;
					case HW_NAPALMVULCAN:
						skillratio += 10*skill_lv-30;
						break;
					case SL_STIN:
						skillratio += (tstatus->size?-99:10*skill_lv); //target size must be small (0) for full damage.
						break;
					case SL_STUN:
						skillratio += (tstatus->size!=2?5*skill_lv:-99); //Full damage is dealt on small/medium targets
						break;
					case SL_SMA:
						skillratio += -60 + status_get_lv(src); //Base damage is 40% + lv%
						break;
					case NJ_KOUENKA:
						skillratio -= 10;
						break;
					case NJ_KAENSIN:
						skillratio -= 50;
						break;
					case NJ_BAKUENRYU:
						skillratio += 50*(skill_lv-1);
						break;
					case NJ_HYOUSYOURAKU:
						skillratio += 50*skill_lv;
						break;
					case NJ_RAIGEKISAI:
						skillratio += 60 + 40*skill_lv;
						break;
					case NJ_KAMAITACHI:
					case NPC_ENERGYDRAIN:
						skillratio += 100*skill_lv;
						break;
				}

				MATK_RATE(skillratio);
			
				//Constant/misc additions from skills
				if (skill_num == WZ_FIREPILLAR)
					MATK_ADD(50);
			}
		}

		if(sd) {
			//Damage bonuses
			if (sd->skillatk[0].id)
			{
				for (i = 0; i < MAX_PC_BONUS && sd->skillatk[i].id && sd->skillatk[i].id != skill_num; i++);
				if (i < MAX_PC_BONUS && sd->skillatk[i].id == skill_num)
					ad.damage += ad.damage*sd->skillatk[i].val/100;
			}

			//Ignore Defense?
			if (!flag.imdef && (
				sd->ignore_mdef_ele & (1<<tstatus->def_ele) ||
				sd->ignore_mdef_race & (1<<tstatus->race) ||
				sd->ignore_mdef_race & (is_boss(target)?1<<RC_BOSS:1<<RC_NONBOSS)
			))
				flag.imdef = 1;
		}

		if(!flag.imdef){
			if(battle_config.magic_defense_type)
				ad.damage = ad.damage - tstatus->mdef*battle_config.magic_defense_type - tstatus->mdef2;
			else
				ad.damage = ad.damage * (100-tstatus->mdef)/100 - tstatus->mdef2;
		}

		if(skill_num == CR_GRANDCROSS || skill_num == NPC_GRANDDARKNESS)
		{	//Apply the physical part of the skill's damage. [Skotlex]
			struct Damage wd = battle_calc_weapon_attack(src,target,skill_num,skill_lv,mflag);
			ad.damage = (wd.damage + ad.damage) * (100 + 40*skill_lv)/100;
			if(src==target)
			{
				if (src->type == BL_PC)
					ad.damage = ad.damage/2;
				else
					ad.damage = 0;
			}
		}

		if(ad.damage<1)
			ad.damage=1;

		if (!(nk&NK_NO_ELEFIX))
			ad.damage=battle_attr_fix(src, target, ad.damage, s_ele, tstatus->def_ele, tstatus->ele_lv);

		if (sd && !(nk&NK_NO_CARDFIX_ATK)) {
			short t_class = status_get_class(target);
			short cardfix=1000;

			cardfix=cardfix*(100+sd->magic_addrace[tstatus->race])/100;
			if (!(nk&NK_NO_ELEFIX))
				cardfix=cardfix*(100+sd->magic_addele[tstatus->def_ele])/100;
			cardfix=cardfix*(100+sd->magic_addsize[tstatus->size])/100;
			cardfix=cardfix*(100+sd->magic_addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
			for(i=0;i<sd->add_mdmg_count;i++) {
				if(sd->add_mdmg[i].class_ == t_class) {
					cardfix=cardfix*(100+sd->add_mdmg[i].rate)/100;
					continue;
				}
			}
			if (cardfix != 1000)
				MATK_RATE(cardfix/10);
		}

		if (tsd && !(nk&NK_NO_CARDFIX_DEF))
	  	{	//Target cards.
			short s_race2=status_get_race2(src);
			short s_class= status_get_class(src);
			short cardfix=1000;

			if (!(nk&NK_NO_ELEFIX))
				cardfix=cardfix*(100-tsd->subele[s_ele])/100;
			cardfix=cardfix*(100-tsd->subsize[sstatus->size])/100;
			cardfix=cardfix*(100-tsd->subrace2[s_race2])/100;
			cardfix=cardfix*(100-tsd->subrace[sstatus->race])/100;
			cardfix=cardfix*(100-tsd->subrace[is_boss(src)?RC_BOSS:RC_NONBOSS])/100;
			for(i=0;i<tsd->add_mdef_count;i++) {
				if(tsd->add_mdef[i].class_ == s_class) {
					cardfix=cardfix*(100-tsd->add_mdef[i].rate)/100;
					continue;
				}
			}
			//It was discovered that ranged defense also counts vs magic! [Skotlex]
			if (ad.flag&BF_SHORT)
				cardfix=cardfix*(100-tsd->near_attack_def_rate)/100;
			else
				cardfix=cardfix*(100-tsd->long_attack_def_rate)/100;

			cardfix=cardfix*(100-tsd->magic_def_rate)/100;

			if (cardfix != 1000)
				MATK_RATE(cardfix/10);
		}
	}

	damage_div_fix(ad.damage, ad.div_);
	
	if (flag.infdef && ad.damage)
		ad.damage = ad.damage>0?1:-1;
		
	ad.damage=battle_calc_damage(src,target,ad.damage,ad.div_,skill_num,skill_lv,ad.flag);
	if (map_flag_gvg2(target->m))
		ad.damage=battle_calc_gvg_damage(src,target,ad.damage,ad.div_,skill_num,skill_lv,ad.flag);
	return ad;
}

/*==========================================
 * »ΜΌ_??[WvZ
 *------------------------------------------
 */
struct Damage  battle_calc_misc_attack(
	struct block_list *src,struct block_list *target,int skill_num,int skill_lv,int mflag)
{
	int skill;
	short i, nk;
	short s_ele;

	struct map_session_data *sd, *tsd;
	struct Damage md; //DO NOT CONFUSE with md of mob_data!
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);

	memset(&md,0,sizeof(md));

	if( src == NULL || target == NULL ){
		nullpo_info(NLP_MARK);
		return md;
	}

	//Some initial values
	md.amotion=skill_get_inf(skill_num)&INF_GROUND_SKILL?0:sstatus->amotion;
	md.dmotion=tstatus->dmotion;
	md.div_=skill_get_num( skill_num,skill_lv );
	md.blewcount=skill_get_blewcount(skill_num,skill_lv);
	md.dmg_lv=ATK_DEF;
	md.flag=BF_MISC|BF_SKILL;

	nk = skill_get_nk(skill_num);
	
	BL_CAST(BL_PC, src, sd);
	BL_CAST(BL_PC, target, tsd);
	
	if(sd) {
		sd->state.arrow_atk = 0;
		md.blewcount += battle_blewcount_bonus(sd, skill_num);
	}

	s_ele = skill_get_pl(skill_num);
	if (s_ele < 0) //Attack that takes weapon's element for misc attacks? Make it neutral [Skotlex]
		s_ele = ELE_NEUTRAL;

	//Skill Range Criteria
	md.flag |= battle_range_type(src, target, skill_num, skill_lv);

	switch(skill_num){
	case HT_LANDMINE:
		md.damage=skill_lv*(sstatus->dex+75)*(100+sstatus->int_)/100;
		break;
	case HT_BLASTMINE:
		md.damage=skill_lv*(sstatus->dex/2+50)*(100+sstatus->int_)/100;
		break;
	case HT_CLAYMORETRAP:
		md.damage=skill_lv*(sstatus->dex/2+75)*(100+sstatus->int_)/100;
		break;
	case HT_BLITZBEAT:
	case SN_FALCONASSAULT:
		//Blitz-beat Damage.
		if(!sd || (skill = pc_checkskill(sd,HT_STEELCROW)) <= 0)
			skill=0;
		md.damage=(sstatus->dex/10+sstatus->int_/2+skill*3+40)*2;
		if(mflag > 1) //Autocasted Blitz.
			nk|=NK_SPLASHSPLIT;
		
		if (skill_num == HT_BLITZBEAT)
			break;
		//Div fix of Blitzbeat
		skill = skill_get_num(HT_BLITZBEAT, 5);
		damage_div_fix(md.damage, skill); 

		//Falcon Assault Modifier
		md.damage=md.damage*(150+70*skill_lv)/100;
		break;
	case TF_THROWSTONE:
		md.damage=50;
		break;
	case BA_DISSONANCE:
		md.damage=30+skill_lv*10;
		if (sd)
			md.damage+= 3*pc_checkskill(sd,BA_MUSICALLESSON);
		break;
	case NPC_SELFDESTRUCTION:
		md.damage = sstatus->hp;
		break;
	case NPC_SMOKING:
		md.damage=3;
		break;
	case NPC_DARKBREATH:
		md.damage = 500 + (skill_lv-1)*1000 + rand()%1000;
		if(md.damage > 9999) md.damage = 9999;
		break;
	case PA_PRESSURE:
		md.damage=500+300*skill_lv;
		break;
	case PA_GOSPEL:
		md.damage = 1+rand()%9999;
		break;
	case CR_ACIDDEMONSTRATION: // updated the formula based on a Japanese formula found to be exact [Reddozen]
		md.damage = 7*tstatus->vit*sstatus->int_*sstatus->int_ / (10*(tstatus->vit+sstatus->int_));
		if (tsd) md.damage>>=1;
		if (md.damage < 0 || md.damage > INT_MAX>>1)
	  	//Overflow prevention, will anyone whine if I cap it to a few billion?
		//Not capped to INT_MAX to give some room for further damage increase.
			md.damage = INT_MAX>>1;
		break;
	case NJ_ZENYNAGE:
		md.damage = skill_get_zeny(skill_num ,skill_lv);
		if (!md.damage) md.damage = 2;
		md.damage = md.damage + rand()%md.damage;
		if (is_boss(target))
			md.damage=md.damage/3;
		else if (tsd)
			md.damage=md.damage/2;
		break;
	case GS_FLING:
		md.damage = sd?sd->status.job_level:status_get_lv(src);
		break;
	case HVAN_EXPLOSION:	//[orn]
		md.damage = sstatus->max_hp * (50 + 50 * skill_lv) / 100 ;
		break ;
	case ASC_BREAKER:
		md.damage = 500+rand()%500 + 5*skill_lv * sstatus->int_;
		nk|=NK_IGNORE_FLEE|NK_NO_ELEFIX; //These two are not properties of the weapon based part.
		break;
	}

	if (nk&NK_SPLASHSPLIT){ // Divide ATK among targets
		if(mflag>0)
			md.damage/= mflag;
		else if(battle_config.error_log)
			ShowError("0 enemies targeted by %s, divide per 0 avoided!\n", skill_get_name(skill_num));
	}

	damage_div_fix(md.damage, md.div_);
	
	if (!(nk&NK_IGNORE_FLEE))
	{
		struct status_change *sc = status_get_sc(target);
		i = 0; //Temp for "hit or no hit"
		if(sc && sc->opt1 && sc->opt1 != OPT1_STONEWAIT)
			i = 1;
		else {
			short
				flee = tstatus->flee,
				hitrate=80; //Default hitrate

			if(battle_config.agi_penalty_type && 
				battle_config.agi_penalty_target&target->type)
			{	
				unsigned char target_count; //256 max targets should be a sane max
				target_count = unit_counttargeted(target,battle_config.agi_penalty_count_lv);
				if(target_count >= battle_config.agi_penalty_count)
				{
					if (battle_config.agi_penalty_type == 1)
						flee = (flee * (100 - (target_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num))/100;
					else //asume type 2: absolute reduction
						flee -= (target_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num;
					if(flee < 1) flee = 1;
				}
			}

			hitrate+= sstatus->hit - flee;

			if (hitrate > battle_config.max_hitrate)
				hitrate = battle_config.max_hitrate;
			else if (hitrate < battle_config.min_hitrate)
				hitrate = battle_config.min_hitrate;

			if(rand()%100 < hitrate)
				i = 1;
		}
		if (!i) {
			md.damage = 0;
			md.dmg_lv=ATK_FLEE;
		}
	}

	if(md.damage && tsd && !(nk&NK_NO_CARDFIX_DEF)){
		int cardfix = 10000;
		int race2 = status_get_race2(src);
		if (!(nk&NK_NO_ELEFIX))
			cardfix=cardfix*(100-tsd->subele[s_ele])/100;
		cardfix=cardfix*(100-tsd->subsize[sstatus->size])/100;
		cardfix=cardfix*(100-tsd->subrace2[race2])/100;
		cardfix=cardfix*(100-tsd->subrace[sstatus->race])/100;
		cardfix=cardfix*(100-tsd->subrace[is_boss(src)?RC_BOSS:RC_NONBOSS])/100;
		cardfix=cardfix*(100-tsd->misc_def_rate)/100;
		if(md.flag&BF_SHORT)
			cardfix=cardfix*(100-tsd->near_attack_def_rate)/100;
		else	// BF_LONG (there's no other choice)
			cardfix=cardfix*(100-tsd->long_attack_def_rate)/100;

		if (cardfix != 10000)
			md.damage=md.damage*cardfix/10000;
	}
	if (sd && skill_num > 0 && sd->skillatk[0].id != 0)
	{
		for (i = 0; i < MAX_PC_BONUS && sd->skillatk[i].id != 0 && sd->skillatk[i].id != skill_num; i++);
		if (i < MAX_PC_BONUS && sd->skillatk[i].id == skill_num)
			md.damage += md.damage*sd->skillatk[i].val/100;
	}

	if(md.damage < 0)
		md.damage = 0;
	else if(md.damage && tstatus->mode&MD_PLANT && skill_num != PA_PRESSURE) //Pressure can vaporize plants
		md.damage = 1;

	if(!(nk&NK_NO_ELEFIX))
		md.damage=battle_attr_fix(src, target, md.damage, s_ele, tstatus->def_ele, tstatus->ele_lv);

	md.damage=battle_calc_damage(src,target,md.damage,md.div_,skill_num,skill_lv,md.flag);
	if (map_flag_gvg2(target->m))
		md.damage=battle_calc_gvg_damage(src,target,md.damage,md.div_,skill_num,skill_lv,md.flag);

	if (skill_num == NJ_ZENYNAGE && sd)
	{	//Time to Pay Up.
		if ( md.damage > sd->status.zeny )
			md.damage=sd->status.zeny;
		pc_payzeny(sd, md.damage);
	}

	return md;
}
/*==========================================
 * _??[WvZκ??p
 *------------------------------------------
 */
struct Damage battle_calc_attack(	int attack_type,
	struct block_list *bl,struct block_list *target,int skill_num,int skill_lv,int flag)
{
	struct Damage d;
	switch(attack_type){
	case BF_WEAPON:
		d = battle_calc_weapon_attack(bl,target,skill_num,skill_lv,flag);
		break;
	case BF_MAGIC:
		d = battle_calc_magic_attack(bl,target,skill_num,skill_lv,flag);
		break;
	case BF_MISC:
		d = battle_calc_misc_attack(bl,target,skill_num,skill_lv,flag);
		break;
	default:
		if (battle_config.error_log)
			ShowError("battle_calc_attack: unknown attack type! %d\n",attack_type);
		memset(&d,0,sizeof(d));
		break;
	}
	if (d.damage + d.damage2 < 1)
	{	//Miss/Absorbed
		//Weapon attacks should go through to cause additional effects.
		if (d.dmg_lv != ATK_LUCKY && attack_type&(BF_MAGIC|BF_MISC))
			d.dmg_lv = ATK_FLEE;
		d.dmotion = 0;
	}
	return d;
}

int battle_calc_return_damage(struct block_list *bl, int *damage, int flag) {
	struct map_session_data *sd=NULL;
	struct status_change *sc;
	int rdamage = 0;

	BL_CAST(BL_PC, bl, sd);
	sc = status_get_sc(bl);

	if(flag&BF_WEAPON) {
		//Bounces back part of the damage.
		if (flag & BF_SHORT) {
			if (sd && sd->short_weapon_damage_return)
			{
				rdamage += *damage * sd->short_weapon_damage_return / 100;
				if(rdamage < 1) rdamage = 1;
			}
			if (sc && sc->data[SC_REFLECTSHIELD].timer != -1)
		  	{
				rdamage += *damage * sc->data[SC_REFLECTSHIELD].val2 / 100;
				if (rdamage < 1) rdamage = 1;
			}
		} else if (flag & BF_LONG) {
			if (sd && sd->long_weapon_damage_return)
			{
				rdamage += *damage * sd->long_weapon_damage_return / 100;
				if (rdamage < 1) rdamage = 1;
			}
		}
	} else
	// magic_damage_return by [AppleGirl] and [Valaris]
	if(flag&BF_MAGIC)
	{
		if(sd && sd->magic_damage_return &&
			rand()%100 < sd->magic_damage_return)
		{	//Bounces back full damage, you take none.
			rdamage = *damage;
		 	*damage = 0;
		}
	}
	return rdamage;
}

void battle_drain(TBL_PC *sd, struct block_list *tbl, int rdamage, int ldamage, int race, int boss)
{
	struct weapon_data *wd;
	int type, thp = 0, tsp = 0, rhp = 0, rsp = 0, hp, sp, i, *damage;
	for (i = 0; i < 4; i++) {
		//First two iterations: Right hand
		if (i < 2) { wd = &sd->right_weapon; damage = &rdamage; }
		else { wd = &sd->left_weapon; damage = &ldamage; }
		if (*damage <= 0) continue;
		//First and Third iterations: race, other two boss/nonboss state
		if (i == 0 || i == 2) 
			type = race;
		else
			type = boss?RC_BOSS:RC_NONBOSS;
		
		hp = wd->hp_drain[type].value;
		if (wd->hp_drain[type].rate)
			hp += battle_calc_drain(*damage,
				wd->hp_drain[type].rate,
		  		wd->hp_drain[type].per);

		sp = wd->sp_drain[type].value;
		if (wd->sp_drain[type].rate)
			sp += battle_calc_drain(*damage,
				wd->sp_drain[type].rate,
			  	wd->sp_drain[type].per);

		if (hp) {
			if (wd->hp_drain[type].type)
				rhp += hp;
			thp += hp;
		}
		if (sp) {
			if (wd->sp_drain[type].type)
				rsp += sp;
			tsp += sp;
		}
	}

	if (sd->sp_vanish_rate && rand()%1000 < sd->sp_vanish_rate)
		status_percent_damage(&sd->bl, tbl, 0, (unsigned char)sd->sp_vanish_per);
	if (!thp && !tsp) return;

	status_heal(&sd->bl, thp, tsp, battle_config.show_hp_sp_drain?3:1);
	
	if (rhp || rsp)
		status_zap(tbl, rhp, rsp);
}

/*==========================================
 * Κ?ν?U??άΖί
 *------------------------------------------
 */
int battle_weapon_attack( struct block_list *src,struct block_list *target,
	 unsigned int tick,int flag)
{
	struct map_session_data *sd = NULL, *tsd = NULL;
	struct status_data *sstatus, *tstatus;
	struct status_change *sc, *tsc;
	int damage,rdamage=0,rdelay=0;
	struct Damage wd;

	nullpo_retr(0, src);
	nullpo_retr(0, target);

	if (src->prev == NULL || target->prev == NULL)
		return 0;

	BL_CAST(BL_PC, src, sd);
	BL_CAST(BL_PC, target, tsd);

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);

	sc = status_get_sc(src);
	tsc = status_get_sc(target);

	if (sc && !sc->count) //Avoid sc checks when there's none to check for. [Skotlex]
		sc = NULL;
	if (tsc && !tsc->count)
		tsc = NULL;
	
	if (sd)
	{
		sd->state.arrow_atk = (sd->status.weapon == W_BOW || (sd->status.weapon >= W_REVOLVER && sd->status.weapon <= W_GRENADE));
		if (sd->state.arrow_atk)
		{	//Recycled damage variable to store index.
			damage = sd->equip_index[EQI_AMMO];
			if (damage<0) {
				clif_arrow_fail(sd,0);
				return 0;
			}
			//Ammo check by Ishizu-chan
			if (sd->inventory_data[damage])
			switch (sd->status.weapon) {
			case W_BOW:
				if (sd->inventory_data[damage]->look != A_ARROW) {
					clif_arrow_fail(sd,0);
					return 0;
				}
			break;
			case W_REVOLVER:
			case W_RIFLE:
			case W_GATLING:
			case W_SHOTGUN:
				if (sd->inventory_data[damage]->look != A_BULLET) {
					clif_arrow_fail(sd,0);
					return 0;
				}
			break;
			case W_GRENADE:
				if (sd->inventory_data[damage]->look != A_GRENADE) {
					clif_arrow_fail(sd,0);
					return 0;
				}
			break;
			}
		}
	}

	if (sc && sc->data[SC_CLOAKING].timer != -1 && !(sc->data[SC_CLOAKING].val4&2))
		status_change_end(src,SC_CLOAKING,-1);

	//Check for counter attacks that block your attack. [Skotlex]
	if(tsc)
	{
		if(tsc->data[SC_AUTOCOUNTER].timer != -1 &&
			(!sc || sc->data[SC_AUTOCOUNTER].timer == -1) &&
			status_check_skilluse(target, src, KN_AUTOCOUNTER, 1)
		)	{
			int dir = map_calc_dir(target,src->x,src->y);
			int t_dir = unit_getdir(target);
			int dist = distance_bl(src, target);
			if(dist <= 0 || (!map_check_dir(dir,t_dir) && dist <= tstatus->rhw.range+1))
			{
				int skilllv = tsc->data[SC_AUTOCOUNTER].val1;
				clif_skillcastcancel(target); //Remove the casting bar. [Skotlex]
				clif_damage(src, target, tick, sstatus->amotion, 1, 0, 1, 0, 0); //Display MISS.
				status_change_end(target,SC_AUTOCOUNTER,-1);
				skill_attack(BF_WEAPON,target,target,src,KN_AUTOCOUNTER,skilllv,tick,0);
				return 0;
			}
		}
		if (tsc->data[SC_BLADESTOP_WAIT].timer != -1 && !is_boss(src)) {
			int skilllv = tsc->data[SC_BLADESTOP_WAIT].val1;
			int duration = skill_get_time2(MO_BLADESTOP,skilllv);
			status_change_end(target, SC_BLADESTOP_WAIT, -1);
			if(sc_start4(src, SC_BLADESTOP, 100, sd?pc_checkskill(sd, MO_BLADESTOP):5, 0, 0, (int)target, duration))
		  	{	//Target locked.
				clif_damage(src, target, tick, sstatus->amotion, 1, 0, 1, 0, 0); //Display MISS.
				clif_bladestop(target,src,1);
				sc_start4(target, SC_BLADESTOP, 100, skilllv, 0, 0,(int)src, duration);
				return 0;
			}
		}
	}
	//Recycled the damage variable rather than use a new one... [Skotlex]
	if(sd && (damage = pc_checkskill(sd,MO_TRIPLEATTACK)) > 0)
	{
		int triple_rate= 30 - damage; //Base Rate
		if (sc && sc->data[SC_SKILLRATE_UP].timer!=-1 && sc->data[SC_SKILLRATE_UP].val1 == MO_TRIPLEATTACK)
		{
			triple_rate+= triple_rate*(sc->data[SC_SKILLRATE_UP].val2)/100;
			status_change_end(src,SC_SKILLRATE_UP,-1);
		}
		if (rand()%100 < triple_rate)
			return skill_attack(BF_WEAPON,src,src,target,MO_TRIPLEATTACK,damage,tick,0);
	}
	else if (sc && sc->data[SC_SACRIFICE].timer != -1)
		return skill_attack(BF_WEAPON,src,src,target,PA_SACRIFICE,sc->data[SC_SACRIFICE].val1,tick,0);

	wd = battle_calc_weapon_attack(src, target, 0, 0, flag);

	if (sd && sd->state.arrow_atk) //Consume arrow.
		battle_consume_ammo(sd, 0, 0);

	damage = wd.damage + wd.damage2;
	if (damage > 0 && src != target) {
		rdamage = battle_calc_return_damage(target, &damage, wd.flag);
		if (rdamage > 0) {
			rdelay = clif_damage(src, src, tick, wd.amotion, sstatus->dmotion, rdamage, 1, 4, 0);
			//Use Reflect Shield to signal this kind of skill trigger. [Skotlex]
			skill_additional_effect(target,src,CR_REFLECTSHIELD, 1,BF_WEAPON,tick);
		}
	}

	wd.dmotion = clif_damage(src, target, tick, wd.amotion, wd.dmotion, wd.damage, wd.div_ , wd.type, wd.damage2);

// Eh, battle_calc_damage should take care of not making the off-hand dmg miss.
//	if(sd && sd->status.weapon > MAX_WEAPON_TYPE && wd.damage2 == 0)
//		clif_damage(src, target, tick+10, wd.amotion, wd.dmotion,0, 1, 0, 0);

	if (sd && sd->splash_range > 0 && damage > 0)
		skill_castend_damage_id(src, target, 0, -1, tick, 0);

	map_freeblock_lock();

	battle_delay_damage(tick+wd.amotion, src, target, BF_WEAPON, 0, 0, damage, wd.dmg_lv, wd.dmotion);

	if (!status_isdead(target) && damage > 0) {
		if (sd) {
			int rate = 0;
			if (sd->weapon_coma_ele[tstatus->def_ele] > 0)
				rate += sd->weapon_coma_ele[tstatus->def_ele];
			if (sd->weapon_coma_race[tstatus->race] > 0)
				rate += sd->weapon_coma_race[tstatus->race];
			if (sd->weapon_coma_race[tstatus->mode&MD_BOSS?RC_BOSS:RC_NONBOSS] > 0)
				rate += sd->weapon_coma_race[tstatus->mode&MD_BOSS?RC_BOSS:RC_NONBOSS];
			if (rate)
				status_change_start(target, SC_COMA, rate, 0, 0, 0, 0, 0, 0);
		}
	}

	if (sc && sc->data[SC_AUTOSPELL].timer != -1 && rand()%100 < sc->data[SC_AUTOSPELL].val4) {
		int sp = 0;
		int skillid = sc->data[SC_AUTOSPELL].val2;
		int skilllv = sc->data[SC_AUTOSPELL].val3;
		int i = rand()%100;
		if (sc->data[SC_SPIRIT].timer != -1 && sc->data[SC_SPIRIT].val2 == SL_SAGE)
			i = 0; //Max chance, no skilllv reduction. [Skotlex]
		if (i >= 50) skilllv -= 2;
		else if (i >= 15) skilllv--;
		if (skilllv < 1) skilllv = 1;
		sp = skill_get_sp(skillid,skilllv) * 2 / 3;

		if (status_charge(src, 0, sp)) {
			switch (skill_get_casttype(skillid)) {
				case CAST_GROUND:
					skill_castend_pos2(src, target->x, target->y, skillid, skilllv, tick, flag);
					break;
				case CAST_NODAMAGE:
					skill_castend_nodamage_id(src, target, skillid, skilllv, tick, flag);
					break;
				case CAST_DAMAGE:
					skill_castend_damage_id(src, target, skillid, skilllv, tick, flag);
					break;
			}
		}
	}
	if (sd) {
		if (wd.flag & BF_WEAPON && src != target && damage > 0) {
			if (battle_config.left_cardfix_to_right)
				battle_drain(sd, target, wd.damage, wd.damage, tstatus->race, is_boss(target));
			else
				battle_drain(sd, target, wd.damage, wd.damage2, tstatus->race, is_boss(target));
		}
	}
	if (rdamage > 0) { //By sending attack type "none" skill_additional_effect won't be invoked. [Skotlex]
		if(tsd && src != target)
			battle_drain(tsd, src, rdamage, rdamage, sstatus->race, is_boss(src));
		battle_delay_damage(tick+wd.amotion, target, src, 0, 0, 0, rdamage, ATK_DEF, rdelay);
	}

	if (tsc) {
		if (tsc->data[SC_POISONREACT].timer != -1 && 
			(rand()%100 < tsc->data[SC_POISONREACT].val3
			|| sstatus->def_ele == ELE_POISON) &&
//			check_distance_bl(src, target, tstatus->rhw.range+1) && Doesn't checks range! o.O;
			status_check_skilluse(target, src, TF_POISON, 0)
		) {	//Poison React
			if (sstatus->def_ele == ELE_POISON) {
				tsc->data[SC_POISONREACT].val2 = 0;
				skill_attack(BF_WEAPON,target,target,src,AS_POISONREACT,tsc->data[SC_POISONREACT].val1,tick,0);
			} else {
				skill_attack(BF_WEAPON,target,target,src,TF_POISON, 5, tick, 0);
				--tsc->data[SC_POISONREACT].val2;
			}
			if (tsc->data[SC_POISONREACT].val2 <= 0)
				status_change_end(target, SC_POISONREACT, -1);
		}
	}
	map_freeblock_unlock();
	return wd.dmg_lv;
}

int battle_check_undead(int race,int element)
{
	if(battle_config.undead_detect_type == 0) {
		if(element == ELE_UNDEAD)
			return 1;
	}
	else if(battle_config.undead_detect_type == 1) {
		if(race == RC_UNDEAD)
			return 1;
	}
	else {
		if(element == ELE_UNDEAD || race == RC_UNDEAD)
			return 1;
	}
	return 0;
}

//Returns the upmost level master starting with the given object
struct block_list* battle_get_master(struct block_list *src)
{
	struct block_list *prev; //Used for infinite loop check (master of yourself?)
	do {
		prev = src;
		switch (src->type) {
			case BL_PET:
				if (((TBL_PET*)src)->msd)
					src = (struct block_list*)((TBL_PET*)src)->msd;
				break;
			case BL_MOB:
				if (((TBL_MOB*)src)->master_id)
					src = map_id2bl(((TBL_MOB*)src)->master_id);
				break;
			case BL_HOM:
				if (((TBL_HOM*)src)->master)
					src = (struct block_list*)((TBL_HOM*)src)->master;
				break;
			case BL_SKILL:
				if (((TBL_SKILL*)src)->group && ((TBL_SKILL*)src)->group->src_id)
					src = map_id2bl(((TBL_SKILL*)src)->group->src_id);
				break;
		}
	} while (src && src != prev);
	return prev;
}

/*==========================================
 * Checks the state between two targets (rewritten by Skotlex)
 * (enemy, friend, party, guild, etc)
 * See battle.h for possible values/combinations
 * to be used here (BCT_* constants)
 * Return value is:
 * 1: flag holds true (is enemy, party, etc)
 * -1: flag fails
 * 0: Invalid target (non-targetable ever)
 *------------------------------------------
 */
int battle_check_target( struct block_list *src, struct block_list *target,int flag)
{
	int m,state = 0; //Initial state none
	int strip_enemy = 1; //Flag which marks whether to remove the BCT_ENEMY status if it's also friend/ally.
	struct block_list *s_bl= src, *t_bl= target;

	nullpo_retr(0, src);
	nullpo_retr(0, target);

	m = target->m;
	if (flag&BCT_ENEMY && !map_flag_gvg(m) && !(status_get_mode(src)&MD_BOSS))
	{	//No offensive stuff while in Basilica.
		if (map_getcell(m,src->x,src->y,CELL_CHKBASILICA) ||
			map_getcell(m,target->x,target->y,CELL_CHKBASILICA))
			return -1;
	}

	//t_bl/s_bl hold the 'master' of the attack, while src/target are the actual
	//objects involved.
	if ((t_bl = battle_get_master(target)) == NULL)
		t_bl = target;

	if ((s_bl = battle_get_master(src)) == NULL)
		s_bl = src;

	switch (target->type)
	{	//Checks on actual target
		case BL_PC:
			if (((TBL_PC*)target)->invincible_timer != -1 || pc_isinvisible((TBL_PC*)target))
				return -1; //Cannot be targeted yet.
			break;
		case BL_MOB:
			if((((TBL_MOB*)target)->special_state.ai == 2 || //Marine Spheres
				(((TBL_MOB*)target)->special_state.ai == 3 && battle_config.summon_flora&1)) && //Floras
				s_bl->type == BL_PC && src->type != BL_MOB)
			{	//Targettable by players
				state |= BCT_ENEMY;
				strip_enemy = 0;
			}
			break;
		case BL_SKILL:
		{
			TBL_SKILL *su = (TBL_SKILL*)target;
			if (!su->group)
				return 0;
			if (skill_get_inf2(su->group->skill_id)&INF2_TRAP)
			{	//Only a few skills can target traps...
				switch (battle_getcurrentskill(src))
				{
					case HT_REMOVETRAP:
					case AC_SHOWER:
					case WZ_HEAVENDRIVE:
						state |= BCT_ENEMY;
						strip_enemy = 0;
						break;
					default:
						return 0;
				}
			} else if (su->group->skill_id==WZ_ICEWALL)
			{	//Icewall can be hit by anything except skills.
				if (src->type == BL_SKILL)
					return 0;
				state |= BCT_ENEMY;
				strip_enemy = 0;
			} else	//Excepting traps and icewall, you should not be able to target skills.
				return 0;
		}
			break;
		//Valid targets with no special checks here.
		case BL_HOM:
			break;
		//All else not specified is an invalid target.
		default:	
			return 0;
	}

	switch (t_bl->type)
	{	//Checks on target master
		case BL_PC:
		{
			TBL_PC *sd = (TBL_PC*)t_bl;
			if (sd->status.karma && t_bl != s_bl && s_bl->type == BL_PC &&
				((TBL_PC*)s_bl)->status.karma)
				state |= BCT_ENEMY; //Characters with bad karma may fight amongst them.
			if (sd->state.monster_ignore && t_bl != s_bl && flag&BCT_ENEMY)
				return 0; //Global inmunity to attacks.
			if (sd->state.killable && t_bl != s_bl)
			{
				state |= BCT_ENEMY; //Universal Victim
				strip_enemy = 0;
			}
			break;
		}
		case BL_MOB:
		{
			TBL_MOB *md = (TBL_MOB*)t_bl;

			if (!(agit_flag && map[m].flag.gvg_castle) && md->guardian_data && md->guardian_data->guild_id)
				return 0; //Disable guardians/emperiums owned by Guilds on non-woe times.
			break;
		}
	}

	switch(src->type)
  	{	//Checks on actual src type
		case BL_PET:
			if (t_bl->type != BL_MOB && flag&BCT_ENEMY)
				return 0; //Pet may not attack non-mobs.
			if (t_bl->type == BL_MOB && ((TBL_MOB*)t_bl)->guardian_data && flag&BCT_ENEMY)
				return 0; //pet may not attack Guardians/Emperium
			break;
		case BL_SKILL:
		{
			struct skill_unit *su = (struct skill_unit *)src;
			if (!su->group)
				return 0;

			//For some mysterious reason ground-skills can't target homun.
			if (target->type == BL_HOM && battle_config.hom_setting&0x2)
				return 0;

			if (su->group->src_id == target->id)
			{
				int inf2;
				inf2 = skill_get_inf2(su->group->skill_id);
				if (inf2&INF2_NO_TARGET_SELF)
					return -1;
				if (inf2&INF2_TARGET_SELF)
					return 1;
			}
			break;
		}
	}

	switch (s_bl->type)
	{	//Checks on source master
		case BL_PC:
		{
			TBL_PC *sd = (TBL_PC*) s_bl;
			if (sd->state.killer && s_bl != t_bl)
			{
				state |= BCT_ENEMY; //Is on a killing rampage :O
				strip_enemy = 0;
			} else
			if (sd->duel_group && t_bl != s_bl && // Duel [LuzZza]
				!(
					(!battle_config.duel_allow_pvp && map[m].flag.pvp) ||
					(!battle_config.duel_allow_gvg && map_flag_gvg(m))
				))
		  	{
				if (t_bl->type == BL_PC &&
					(sd->duel_group == ((TBL_PC*)t_bl)->duel_group))
					//Duel targets can ONLY be your enemy, nothing else.
					return (BCT_ENEMY&flag)?1:-1;
				else // You can't target anything out of your duel
					return 0;
			}
			if (map_flag_gvg(m) && !sd->status.guild_id &&
				t_bl->type == BL_MOB && ((TBL_MOB*)t_bl)->guardian_data)
				return 0; //If you don't belong to a guild, can't target guardians/emperium.
			if (t_bl->type != BL_PC)
				state |= BCT_ENEMY; //Natural enemy.
			break;
		}
		case BL_MOB:
		{
			TBL_MOB*md = (TBL_MOB*)s_bl;
			if (!(agit_flag && map[m].flag.gvg_castle) && md->guardian_data && md->guardian_data->guild_id)
				return 0; //Disable guardians/emperium owned by Guilds on non-woe times.
				if (!md->special_state.ai) { //Normal mobs.
					if (t_bl->type == BL_MOB && !((TBL_MOB*)t_bl)->special_state.ai)
						state |= BCT_PARTY; //Normal mobs with no ai are friends.
					else
						state |= BCT_ENEMY; //However, all else are enemies.
				} else {
					if (t_bl->type == BL_MOB && !((TBL_MOB*)t_bl)->special_state.ai)
						state |= BCT_ENEMY; //Natural enemy for AI mobs are normal mobs.
				}
			break;
		}
		default:
		//Need some sort of default behaviour for unhandled types.
			if (t_bl->type != s_bl->type)
				state |= BCT_ENEMY;
			break;
	}
	
	if ((flag&BCT_ALL) == BCT_ALL) { //All actually stands for all attackable chars 
		if (target->type&BL_CHAR)
			return 1;
		else
			return -1;
	} else
	if (flag == BCT_NOONE) //Why would someone use this? no clue.
		return -1;
	
	if (t_bl == s_bl)
	{	//No need for further testing.
		state |= BCT_SELF|BCT_PARTY|BCT_GUILD;
		if (state&BCT_ENEMY && strip_enemy)
			state&=~BCT_ENEMY;
		return (flag&state)?1:-1;
	}
	
	if (map_flag_vs(m)) { //Check rivalry settings.
		if (flag&(BCT_PARTY|BCT_ENEMY)) {
			int s_party = status_get_party_id(s_bl);
			if (
				!(map[m].flag.pvp && map[m].flag.pvp_noparty) &&
				!(map_flag_gvg(m) && map[m].flag.gvg_noparty) &&
				s_party && s_party == status_get_party_id(t_bl)
			)
				state |= BCT_PARTY;
			else
				state |= BCT_ENEMY;
		}
		if (flag&(BCT_GUILD|BCT_ENEMY)) {
			int s_guild = status_get_guild_id(s_bl);
			int t_guild = status_get_guild_id(t_bl);
			if (
				!(map[m].flag.pvp && map[m].flag.pvp_noguild) &&
				s_guild && t_guild && (s_guild == t_guild || guild_idisallied(s_guild, t_guild))
			)
				state |= BCT_GUILD;
			else
				state |= BCT_ENEMY;
		}
		if (state&BCT_ENEMY && battle_config.pk_mode && !map_flag_gvg(m) &&
			s_bl->type == BL_PC && t_bl->type == BL_PC)
		{	//Prevent novice engagement on pk_mode (feature by Valaris)
			TBL_PC *sd = (TBL_PC*)s_bl, *sd2 = (TBL_PC*)t_bl;
			if (
				(sd->class_&MAPID_UPPERMASK) == MAPID_NOVICE ||
				(sd2->class_&MAPID_UPPERMASK) == MAPID_NOVICE ||
				sd->status.base_level < battle_config.pk_min_level ||
			  	sd2->status.base_level < battle_config.pk_min_level ||
				(battle_config.pk_level_range && (
					sd->status.base_level > sd2->status.base_level ?
					sd->status.base_level - sd2->status.base_level :
					sd2->status.base_level - sd->status.base_level )
			  		> battle_config.pk_level_range)
			)
				state&=~BCT_ENEMY;
		}
	} else { //Non pvp/gvg, check party/guild settings.
		if (flag&BCT_PARTY || state&BCT_ENEMY) {
			int s_party = status_get_party_id(s_bl);
			if(s_party && s_party == status_get_party_id(t_bl))
				state |= BCT_PARTY;
		}
		if (flag&BCT_GUILD || state&BCT_ENEMY) {
			int s_guild = status_get_guild_id(s_bl);
			int t_guild = status_get_guild_id(t_bl);
			if(s_guild && t_guild && (s_guild == t_guild || guild_idisallied(s_guild, t_guild)))
				state |= BCT_GUILD;
		}
	}
	
	if (!state) //If not an enemy, nor a guild, nor party, nor yourself, it's neutral.
		state = BCT_NEUTRAL;
	//Alliance state takes precedence over enemy one.
	else if (state&BCT_ENEMY && strip_enemy && state&(BCT_SELF|BCT_PARTY|BCT_GUILD))
		state&=~BCT_ENEMY;

	return (flag&state)?1:-1;
}
/*==========================================
 * Λφ»θ
 *------------------------------------------
 */
int battle_check_range(struct block_list *src,struct block_list *bl,int range)
{
	nullpo_retr(0, src);
	nullpo_retr(0, bl);

	if(src->m != bl->m)	// α€}bv
		return 0;

	if (!check_distance_bl(src, bl, range))
		return 0;

	if(distance_bl(src, bl) < 2) //No need for path checking.
		return 1;

	// ?αQ¨»θ
	return path_search_long(NULL,src->m,src->x,src->y,bl->x,bl->y);
}

/*==========================================
 * Return numerical value of a switch configuration (modified by [Yor])
 * on/off, english, franηais, deutsch, espaρol
 *------------------------------------------
 */
int battle_config_switch(const char *str) {
	if(strncmpi(str, "on",2) == 0 ||
		strncmpi(str, "yes",3) == 0 ||
		strncmpi(str, "oui",3) == 0 ||
		strncmpi(str, "ja",2) == 0 ||
		strncmpi(str, "si",2) == 0)
		return 1;
	if(strncmpi(str, "off",3) == 0 ||
		strncmpi(str, "no",2) == 0 ||
		strncmpi(str, "non",3) == 0 ||
		strncmpi(str, "nein",4) == 0)
		return 0;
	return (int)strtol(str,NULL,0);
}

static const struct battle_data_short {
	const char *str;
	unsigned short *val;
} battle_data_short[] = {	//List here battle_athena options which are type unsigned short!
	{ "warp_point_debug",                  &battle_config.warp_point_debug			},
	{ "enable_critical",                   &battle_config.enable_critical	},
	{ "mob_critical_rate",                 &battle_config.mob_critical_rate		},
	{ "critical_rate",                     &battle_config.critical_rate		},
	{ "enable_baseatk",                    &battle_config.enable_baseatk				},
	{ "enable_perfect_flee",               &battle_config.enable_perfect_flee		},
	{ "casting_rate",                      &battle_config.cast_rate				},
	{ "delay_rate",                        &battle_config.delay_rate				},
	{ "delay_dependon_agi",                &battle_config.delay_dependon_agi },
	{ "skill_delay_attack_enable",         &battle_config.sdelay_attack_enable		},
	{ "left_cardfix_to_right",             &battle_config.left_cardfix_to_right	},
	{ "skill_add_range",            			&battle_config.skill_add_range		},
	{ "skill_out_range_consume",           &battle_config.skill_out_range_consume	},
	{ "skillrange_by_distance",            &battle_config.skillrange_by_distance	},
	{ "skillrange_from_weapon",            &battle_config.use_weapon_skill_range  },
	{ "player_damage_delay_rate",          &battle_config.pc_damage_delay_rate		},
	{ "defunit_not_enemy",                 &battle_config.defnotenemy				},
	{ "gvg_traps_target_all",	            &battle_config.vs_traps_bctall			},
	{ "traps_setting",	                  &battle_config.traps_setting	},
	{ "summon_flora_setting",              &battle_config.summon_flora	},
	{ "clear_skills_on_death",             &battle_config.clear_unit_ondeath },
	{ "clear_skills_on_warp",              &battle_config.clear_unit_onwarp },
	{ "random_monster_checklv",            &battle_config.random_monster_checklv	},
	{ "attribute_recover",                 &battle_config.attr_recover				},
	{ "item_auto_get",                     &battle_config.item_auto_get			},
	{ "drop_rate0item",                    &battle_config.drop_rate0item			},
	{ "pvp_exp",                           &battle_config.pvp_exp		},
	{ "gtb_sc_immunity",                   &battle_config.gtb_sc_immunity},
	{ "guild_max_castles",                 &battle_config.guild_max_castles		},
	{ "emergency_call",                    &battle_config.emergency_call },
	{ "guild_aura",                        &battle_config.guild_aura	},
	{ "death_penalty_type",                &battle_config.death_penalty_type		},
	{ "death_penalty_base",                &battle_config.death_penalty_base		},
	{ "death_penalty_job",                 &battle_config.death_penalty_job		},
	{ "restart_hp_rate",                   &battle_config.restart_hp_rate			},
	{ "restart_sp_rate",                   &battle_config.restart_sp_rate			},
	{ "mvp_hp_rate",                       &battle_config.mvp_hp_rate				},
	{ "monster_hp_rate",                   &battle_config.monster_hp_rate			},
	{ "monster_max_aspd",                  &battle_config.monster_max_aspd			},
	{ "view_range_rate",                   &battle_config.view_range_rate },
	{ "chase_range_rate",                  &battle_config.chase_range_rate },
	{ "atcommand_gm_only",                 &battle_config.atc_gmonly				},
	{ "atcommand_spawn_quantity_limit",    &battle_config.atc_spawn_quantity_limit	},
	{ "atcommand_slave_clone_limit",       &battle_config.atc_slave_clone_limit},
	{ "partial_name_scan",                 &battle_config.partial_name_scan	},
	{ "gm_all_skill",                      &battle_config.gm_allskill				},
	{ "gm_all_equipment",                  &battle_config.gm_allequip				},
	{ "gm_skill_unconditional",            &battle_config.gm_skilluncond			},
	{ "gm_join_chat",                      &battle_config.gm_join_chat				},
	{ "gm_kick_chat",                      &battle_config.gm_kick_chat				},
	{ "player_skillfree",                  &battle_config.skillfree				},
	{ "player_skillup_limit",              &battle_config.skillup_limit			},
	{ "weapon_produce_rate",               &battle_config.wp_rate					},
	{ "potion_produce_rate",               &battle_config.pp_rate					},
	{ "monster_active_enable",             &battle_config.monster_active_enable	},
	{ "monster_damage_delay_rate",         &battle_config.monster_damage_delay_rate},
	{ "monster_loot_type",                 &battle_config.monster_loot_type		},
//	{ "mob_skill_use",                     &battle_config.mob_skill_use			},	//Deprecated
	{ "mob_skill_rate",                    &battle_config.mob_skill_rate			},
	{ "mob_skill_delay",                   &battle_config.mob_skill_delay			},
	{ "mob_count_rate",                    &battle_config.mob_count_rate			},
	{ "mob_spawn_delay",                   &battle_config.mob_spawn_delay			},
	{ "no_spawn_on_player",                &battle_config.no_spawn_on_player	},
	{ "force_random_spawn",                &battle_config.force_random_spawn	},
	{ "plant_spawn_delay",                 &battle_config.plant_spawn_delay			},
	{ "boss_spawn_delay",                  &battle_config.boss_spawn_delay			},
	{ "slaves_inherit_mode",               &battle_config.slaves_inherit_mode	},
	{ "slaves_inherit_speed",              &battle_config.slaves_inherit_speed		},
	{ "summons_trigger_autospells",           &battle_config.summons_trigger_autospells	},
	{ "pc_damage_walk_delay_rate",         &battle_config.pc_walk_delay_rate		},
	{ "damage_walk_delay_rate",            &battle_config.walk_delay_rate		},
	{ "multihit_delay",                    &battle_config.multihit_delay			},
	{ "quest_skill_learn",                 &battle_config.quest_skill_learn		},
	{ "quest_skill_reset",                 &battle_config.quest_skill_reset		},
	{ "basic_skill_check",                 &battle_config.basic_skill_check		},
	{ "guild_emperium_check",              &battle_config.guild_emperium_check		},
	{ "guild_exp_limit",                   &battle_config.guild_exp_limit			},
	{ "player_invincible_time",            &battle_config.pc_invincible_time		},
	{ "pet_catch_rate",                    &battle_config.pet_catch_rate			},
	{ "pet_rename",                        &battle_config.pet_rename				},
	{ "pet_friendly_rate",                 &battle_config.pet_friendly_rate		},
	{ "pet_hungry_delay_rate",             &battle_config.pet_hungry_delay_rate	},
	{ "pet_hungry_friendly_decrease",      &battle_config.pet_hungry_friendly_decrease},
	{ "pet_status_support",                &battle_config.pet_status_support		},
	{ "pet_attack_support",                &battle_config.pet_attack_support		},
	{ "pet_damage_support",                &battle_config.pet_damage_support		},
	{ "pet_support_min_friendly",          &battle_config.pet_support_min_friendly	},
	{ "pet_support_rate",                  &battle_config.pet_support_rate			},
	{ "pet_attack_exp_to_master",          &battle_config.pet_attack_exp_to_master	},
	{ "pet_attack_exp_rate",               &battle_config.pet_attack_exp_rate	 },
	{ "pet_lv_rate",                       &battle_config.pet_lv_rate				},	//Skotlex
	{ "pet_max_stats",                     &battle_config.pet_max_stats				},	//Skotlex
	{ "pet_max_atk1",                      &battle_config.pet_max_atk1				},	//Skotlex
	{ "pet_max_atk2",                      &battle_config.pet_max_atk2				},	//Skotlex
	{ "pet_disable_in_gvg",                        &battle_config.pet_no_gvg					},	//Skotlex
	{ "skill_min_damage",                  &battle_config.skill_min_damage			},
	{ "finger_offensive_type",             &battle_config.finger_offensive_type	},
	{ "heal_exp",                          &battle_config.heal_exp					},
	{ "max_heal_lv",                       &battle_config.max_heal_lv	},
	{ "resurrection_exp",                  &battle_config.resurrection_exp			},
	{ "shop_exp",                          &battle_config.shop_exp					},
	{ "combo_delay_rate",                  &battle_config.combo_delay_rate			},
	{ "item_check",                        &battle_config.item_check				},
	{ "item_use_interval",                 &battle_config.item_use_interval	},
	{ "wedding_modifydisplay",             &battle_config.wedding_modifydisplay	},
	{ "wedding_ignorepalette",             &battle_config.wedding_ignorepalette	},	//[Skotlex]
	{ "xmas_ignorepalette",                &battle_config.xmas_ignorepalette	},	// [Valaris]
	{ "natural_heal_weight_rate",          &battle_config.natural_heal_weight_rate	},
	{ "arrow_decrement",                   &battle_config.arrow_decrement			},
	{ "max_aspd",                          &battle_config.max_aspd					},
	{ "max_walk_speed",                    &battle_config.max_walk_speed			},
	{ "max_lv",                            &battle_config.max_lv					},
	{ "aura_lv",                           &battle_config.aura_lv					},
	{ "max_parameter",                     &battle_config.max_parameter			},
	{ "max_baby_parameter",                &battle_config.max_baby_parameter	},
	{ "max_def",                           &battle_config.max_def					},
	{ "over_def_bonus",                    &battle_config.over_def_bonus			},
	{ "skill_log",                         &battle_config.skill_log			},
	{ "battle_log",                        &battle_config.battle_log				},
	{ "save_log",                          &battle_config.save_log					},
	{ "error_log",                         &battle_config.error_log				},
	{ "etc_log",                           &battle_config.etc_log					},
	{ "save_clothcolor",                   &battle_config.save_clothcolor			},
	{ "undead_detect_type",                &battle_config.undead_detect_type		},
	{ "auto_counter_type",                 &battle_config.auto_counter_type		},
	{ "min_hitrate",                       &battle_config.min_hitrate	},
	{ "max_hitrate",                       &battle_config.max_hitrate	},
	{ "agi_penalty_target",                &battle_config.agi_penalty_target	},
	{ "agi_penalty_type",                  &battle_config.agi_penalty_type			},
	{ "agi_penalty_count",                 &battle_config.agi_penalty_count			},
	{ "agi_penalty_num",                   &battle_config.agi_penalty_num			},
	{ "agi_penalty_count_lv",              &battle_config.agi_penalty_count_lv		},
	{ "vit_penalty_target",                &battle_config.vit_penalty_target },
	{ "vit_penalty_type",                  &battle_config.vit_penalty_type			},
	{ "vit_penalty_count",                 &battle_config.vit_penalty_count			},
	{ "vit_penalty_num",                   &battle_config.vit_penalty_num			},
	{ "vit_penalty_count_lv",              &battle_config.vit_penalty_count_lv		},
	{ "weapon_defense_type",               &battle_config.weapon_defense_type		},
	{ "magic_defense_type",                &battle_config.magic_defense_type		},
	{ "skill_reiteration",                 &battle_config.skill_reiteration		},
	{ "skill_nofootset",                   &battle_config.skill_nofootset		},
	{ "player_cloak_check_type",           &battle_config.pc_cloak_check_type		},
	{ "monster_cloak_check_type",          &battle_config.monster_cloak_check_type	},
	{ "sense_type",                        &battle_config.estimation_type },
	{ "gvg_short_attack_damage_rate",      &battle_config.gvg_short_damage_rate	},
	{ "gvg_long_attack_damage_rate",       &battle_config.gvg_long_damage_rate		},
	{ "gvg_weapon_attack_damage_rate",     &battle_config.gvg_weapon_damage_rate	},
	{ "gvg_magic_attack_damage_rate",      &battle_config.gvg_magic_damage_rate	},
	{ "gvg_misc_attack_damage_rate",       &battle_config.gvg_misc_damage_rate		},
	{ "gvg_flee_penalty",                  &battle_config.gvg_flee_penalty			},
	{ "pk_short_attack_damage_rate",      &battle_config.pk_short_damage_rate	},
	{ "pk_long_attack_damage_rate",       &battle_config.pk_long_damage_rate		},
	{ "pk_weapon_attack_damage_rate",     &battle_config.pk_weapon_damage_rate	},
	{ "pk_magic_attack_damage_rate",      &battle_config.pk_magic_damage_rate	},
	{ "pk_misc_attack_damage_rate",       &battle_config.pk_misc_damage_rate		},
	{ "mob_changetarget_byskill",          &battle_config.mob_changetarget_byskill},
	{ "attack_direction_change",           &battle_config.attack_direction_change },
	{ "land_skill_limit",                  &battle_config.land_skill_limit		},
	{ "party_skill_penalty",               &battle_config.party_skill_penalty		},
	{ "monster_class_change_full_recover", &battle_config.monster_class_change_full_recover },
	{ "produce_item_name_input",           &battle_config.produce_item_name_input	},
	{ "display_skill_fail",                &battle_config.display_skill_fail	},
	{ "chat_warpportal",                   &battle_config.chat_warpportal			},
	{ "mob_warp",                          &battle_config.mob_warp	},
	{ "dead_branch_active",                &battle_config.dead_branch_active			},
	{ "show_steal_in_same_party",          &battle_config.show_steal_in_same_party		},
	{ "party_hp_mode",                     &battle_config.party_hp_mode },
	{ "show_party_share_picker",           &battle_config.party_show_share_picker },
	{ "party_update_interval",             &battle_config.party_update_interval },
	{ "party_item_share_type",             &battle_config.party_share_type },
	{ "attack_attr_none",                  &battle_config.attack_attr_none		},
	{ "gx_allhit",                         &battle_config.gx_allhit				},
	{ "gx_disptype",                       &battle_config.gx_disptype				},
	{ "devotion_level_difference",         &battle_config.devotion_level_difference	},
	{ "player_skill_partner_check",        &battle_config.player_skill_partner_check},
	{ "hide_GM_session",                   &battle_config.hide_GM_session			},
	{ "invite_request_check",              &battle_config.invite_request_check		},
	{ "skill_removetrap_type",             &battle_config.skill_removetrap_type	},
	{ "disp_experience",                   &battle_config.disp_experience			},
	{ "disp_zeny",                         &battle_config.disp_zeny				},
	{ "castle_defense_rate",               &battle_config.castle_defense_rate		},
	{ "hp_rate",                           &battle_config.hp_rate					},
	{ "sp_rate",                           &battle_config.sp_rate					},
	{ "gm_cant_drop_min_lv",                    &battle_config.gm_cant_drop_min_lv			},
	{ "gm_cant_drop_max_lv",                    &battle_config.gm_cant_drop_max_lv			},
	{ "disp_hpmeter",                      &battle_config.disp_hpmeter				},
	{ "bone_drop",		                   &battle_config.bone_drop				},
	{ "buyer_name",                        &battle_config.buyer_name		},
	{ "skill_wall_check",                  &battle_config.skill_wall_check     },
	{ "cell_stack_limit",                  &battle_config.cell_stack_limit     },
// eAthena additions
	{ "item_logarithmic_drops",            &battle_config.logarithmic_drops	},
	{ "item_drop_common_min",              &battle_config.item_drop_common_min	},	// Added by TyrNemesis^
	{ "item_drop_common_max",              &battle_config.item_drop_common_max	},
	{ "item_drop_equip_min",               &battle_config.item_drop_equip_min	},
	{ "item_drop_equip_max",               &battle_config.item_drop_equip_max	},
	{ "item_drop_card_min",                &battle_config.item_drop_card_min	},
	{ "item_drop_card_max",                &battle_config.item_drop_card_max	},
	{ "item_drop_mvp_min",                 &battle_config.item_drop_mvp_min	},
	{ "item_drop_mvp_max",                 &battle_config.item_drop_mvp_max	},	// End Addition
	{ "item_drop_heal_min",                &battle_config.item_drop_heal_min },
	{ "item_drop_heal_max",                &battle_config.item_drop_heal_max },
	{ "item_drop_use_min",                 &battle_config.item_drop_use_min },
	{ "item_drop_use_max",                 &battle_config.item_drop_use_max },
	{ "item_drop_add_min",                 &battle_config.item_drop_adddrop_min },
	{ "item_drop_add_max",                 &battle_config.item_drop_adddrop_max },
	{ "item_drop_treasure_min",            &battle_config.item_drop_treasure_min },
	{ "item_drop_treasure_max",            &battle_config.item_drop_treasure_max },
	{ "prevent_logout",                    &battle_config.prevent_logout		},	// Added by RoVeRT
	{ "alchemist_summon_reward",           &battle_config.alchemist_summon_reward	},	// [Valaris]
	{ "drops_by_luk",                      &battle_config.drops_by_luk	},	// [Valaris]
	{ "drops_by_luk2",                     &battle_config.drops_by_luk2	},	// [Skotlex]
	{ "equip_natural_break_rate",          &battle_config.equip_natural_break_rate	},
	{ "equip_self_break_rate",             &battle_config.equip_self_break_rate	},
	{ "equip_skill_break_rate",            &battle_config.equip_skill_break_rate	},
	{ "pk_mode",                           &battle_config.pk_mode			},  	// [Valaris]
	{ "pk_level_range",                    &battle_config.pk_level_range	},
	{ "manner_system",                     &battle_config.manner_system		},  	// [Komurka]
	{ "pet_equip_required",                &battle_config.pet_equip_required	},	// [Valaris]
	{ "multi_level_up",                    &battle_config.multi_level_up		}, // [Valaris]
	{ "max_exp_gain_rate",                 &battle_config.max_exp_gain_rate	}, // [Skotlex]
	{ "backstab_bow_penalty",              &battle_config.backstab_bow_penalty	},
	{ "night_at_start",                    &battle_config.night_at_start	}, // added by [Yor]
	{ "show_mob_info",                     &battle_config.show_mob_info }, // [Valaris]
	{ "hack_info_GM_level",                &battle_config.hack_info_GM_level	}, // added by [Yor]
	{ "any_warp_GM_min_level",             &battle_config.any_warp_GM_min_level	}, // added by [Yor]
	{ "packet_ver_flag",                   &battle_config.packet_ver_flag	}, // added by [Yor]
	{ "min_hair_style",                    &battle_config.min_hair_style	}, // added by [MouseJstr]
	{ "max_hair_style",                    &battle_config.max_hair_style	}, // added by [MouseJstr]
	{ "min_hair_color",                    &battle_config.min_hair_color	}, // added by [MouseJstr]
	{ "max_hair_color",                    &battle_config.max_hair_color	}, // added by [MouseJstr]
	{ "min_cloth_color",                   &battle_config.min_cloth_color	}, // added by [MouseJstr]
	{ "max_cloth_color",                   &battle_config.max_cloth_color	}, // added by [MouseJstr]
	{ "pet_hair_style",                    &battle_config.pet_hair_style	}, // added by [Skotlex]
	{ "castrate_dex_scale",                &battle_config.castrate_dex_scale	}, // added by [MouseJstr]
	{ "area_size",                         &battle_config.area_size	}, // added by [MouseJstr]
	{ "zeny_from_mobs",                    &battle_config.zeny_from_mobs}, // [Valaris]
	{ "mobs_level_up",                     &battle_config.mobs_level_up}, // [Valaris]
	{ "mobs_level_up_exp_rate",		   &battle_config.mobs_level_up_exp_rate}, // [Valaris]
	{ "pk_min_level",                      &battle_config.pk_min_level}, // [celest]
	{ "skill_steal_type",                  &battle_config.skill_steal_type}, // [celest]
	{ "skill_steal_rate",                  &battle_config.skill_steal_rate}, // [celest]
	{ "skill_steal_max_tries",             &battle_config.skill_steal_max_tries}, // [Lupus]
	{ "motd_type",                         &battle_config.motd_type}, // [celest]
	{ "finding_ore_rate",                  &battle_config.finding_ore_rate}, // [celest]
	{ "exp_calc_type",                     &battle_config.exp_calc_type}, // [celest]
	{ "exp_bonus_attacker",                &battle_config.exp_bonus_attacker}, // [Skotlex]
	{ "exp_bonus_max_attacker",            &battle_config.exp_bonus_max_attacker}, // [Skotlex]
	{ "min_skill_delay_limit",             &battle_config.min_skill_delay_limit}, // [celest]
	{ "default_skill_delay",               &battle_config.default_skill_delay}, // [Skotlex]
	{ "no_skill_delay",                    &battle_config.no_skill_delay}, // [Skotlex]
	{ "attack_walk_delay",                 &battle_config.attack_walk_delay }, // [Skotlex]
	{ "require_glory_guild",               &battle_config.require_glory_guild}, // [celest]
	{ "idle_no_share",                     &battle_config.idle_no_share}, // [celest], for a feature by [MouseJstr]
	{ "party_even_share_bonus",            &battle_config.party_even_share_bonus}, 
	{ "delay_battle_damage",               &battle_config.delay_battle_damage}, // [celest]
	{ "hide_woe_damage",                   &battle_config.hide_woe_damage}, // [Skotlex]
	{ "display_version",	                  &battle_config.display_version}, // [Ancyker], for a feature by...?
	{ "display_site",	                  &battle_config.display_site}, // [Mehah]
	{ "who_display_aid",	                  &battle_config.who_display_aid}, // [Ancyker], for a feature by...?
	{ "display_hallucination",             &battle_config.display_hallucination}, // [Skotlex]
	{ "use_statpoint_table",               &battle_config.use_statpoint_table}, // [Skotlex]
	{ "ignore_items_gender",               &battle_config.ignore_items_gender}, // [Lupus]
	{ "copyskill_restrict",		       &battle_config.copyskill_restrict}, // [Aru]
	{ "berserk_cancels_buffs",		&battle_config.berserk_cancels_buffs}, // [Aru]

	{ "debuff_on_logout",                  &battle_config.debuff_on_logout},
	{ "monster_ai",                        &battle_config.mob_ai},
	{ "hom_setting",                        &battle_config.hom_setting},
	{ "dynamic_mobs",                      &battle_config.dynamic_mobs},
	{ "mob_remove_damaged",                &battle_config.mob_remove_damaged},
	{ "show_hp_sp_drain",                  &battle_config.show_hp_sp_drain}, // [Skotlex]
	{ "show_hp_sp_gain",                   &battle_config.show_hp_sp_gain}, // [Skotlex]
	{ "mob_npc_event_type",                &battle_config.mob_npc_event_type},
	{ "mob_clear_delay",                   &battle_config.mob_clear_delay}, // [Valaris]
	{ "character_size",						&battle_config.character_size}, // [Lupus]
	{ "mob_max_skilllvl",				&battle_config.mob_max_skilllvl}, // [Lupus]
	{ "retaliate_to_master",			&battle_config.retaliate_to_master}, // [Skotlex]
	{ "rare_drop_announce",				&battle_config.rare_drop_announce}, // [Lupus]
	{ "firewall_hits_on_undead",			&battle_config.firewall_hits_on_undead}, // [Skotlex]
	{ "title_lvl1",				&battle_config.title_lvl1}, // [Lupus]
	{ "title_lvl2",				&battle_config.title_lvl2}, // [Lupus]
	{ "title_lvl3",				&battle_config.title_lvl3}, // [Lupus]
	{ "title_lvl4",				&battle_config.title_lvl4}, // [Lupus]
	{ "title_lvl5",				&battle_config.title_lvl5}, // [Lupus]
	{ "title_lvl6",				&battle_config.title_lvl6}, // [Lupus]
	{ "title_lvl7",				&battle_config.title_lvl7}, // [Lupus]
	{ "title_lvl8",				&battle_config.title_lvl8}, // [Lupus]
	
	{ "duel_enable",						&battle_config.duel_enable}, // [LuzZza]
	{ "duel_allow_pvp",						&battle_config.duel_allow_pvp}, // [LuzZza]
	{ "duel_allow_gvg",						&battle_config.duel_allow_gvg}, // [LuzZza]
	{ "duel_allow_teleport",				&battle_config.duel_allow_teleport}, // [LuzZza]
	{ "duel_autoleave_when_die",			&battle_config.duel_autoleave_when_die}, //[LuzZza]
	{ "duel_time_interval",					&battle_config.duel_time_interval}, // [LuzZza]
	{ "duel_only_on_same_map",				&battle_config.duel_only_on_same_map}, // [Toms]
	{ "skip_teleport_lv1_menu",			&battle_config.skip_teleport_lv1_menu}, // [LuzZza]
	{ "allow_skill_without_day",			&battle_config.allow_skill_without_day}, // [Komurka]
	{ "allow_es_magic_player",				&battle_config.allow_es_magic_pc },
	{ "skill_caster_check",					&battle_config.skill_caster_check },
	{ "status_cast_cancel",					&battle_config.sc_castcancel },
	{ "pc_status_def_rate",					&battle_config.pc_sc_def_rate },
	{ "mob_status_def_rate",				&battle_config.mob_sc_def_rate },
	{ "pc_luk_status_def",					&battle_config.pc_luk_sc_def },
	{ "mob_luk_status_def",					&battle_config.mob_luk_sc_def },
	{ "pc_max_status_def",					&battle_config.pc_max_sc_def },
	{ "mob_max_status_def",					&battle_config.mob_max_sc_def },
	{ "sg_miracle_skill_ratio",			&battle_config.sg_miracle_skill_ratio },
	{ "sg_angel_skill_ratio",		 		&battle_config.sg_angel_skill_ratio },
	{ "autospell_stacking", 				&battle_config.autospell_stacking },
	{ "override_mob_names", 				&battle_config.override_mob_names },
	{ "min_chat_delay",						&battle_config.min_chat_delay },
	{ "block_area",                           &battle_config.block_area		}, // [Kamper]
	{ "friend_auto_add",						&battle_config.friend_auto_add },
	{ "hom_rename",                     &battle_config.hom_rename },
	{ "homunculus_show_growth",					&battle_config.homunculus_show_growth },	//[orn]
	{ "homunculus_friendly_rate",				&battle_config.homunculus_friendly_rate },
};

static const struct battle_data_int {
	const char *str;
	int *val;
} battle_data_int[] = {	//List here battle_athena options which are type int!
	{ "flooritem_lifetime",                &battle_config.flooritem_lifetime		},
	{ "item_first_get_time",               &battle_config.item_first_get_time		},
	{ "item_second_get_time",              &battle_config.item_second_get_time		},
	{ "item_third_get_time",               &battle_config.item_third_get_time		},
	{ "mvp_item_first_get_time",           &battle_config.mvp_item_first_get_time	},
	{ "mvp_item_second_get_time",          &battle_config.mvp_item_second_get_time	},
	{ "mvp_item_third_get_time",           &battle_config.mvp_item_third_get_time	},
	{ "base_exp_rate",                     &battle_config.base_exp_rate			},
	{ "job_exp_rate",                      &battle_config.job_exp_rate				},
	{ "zeny_penalty",                      &battle_config.zeny_penalty				},
	{ "mvp_exp_rate",                      &battle_config.mvp_exp_rate				},
	{ "natural_healhp_interval",           &battle_config.natural_healhp_interval	},
	{ "natural_healsp_interval",           &battle_config.natural_healsp_interval	},
	{ "natural_heal_skill_interval",       &battle_config.natural_heal_skill_interval},
	{ "max_hp",                            &battle_config.max_hp					},
	{ "max_sp",                            &battle_config.max_sp					},
	{ "max_cart_weight",                   &battle_config.max_cart_weight			},
	{ "gvg_eliminate_time",                &battle_config.gvg_eliminate_time		},
	{ "vending_max_value",                 &battle_config.vending_max_value		},
// eAthena additions
	{ "item_rate_mvp",                     &battle_config.item_rate_mvp		},
	{ "item_rate_common",                  &battle_config.item_rate_common	},	// Added by RoVeRT
	{ "item_rate_common_boss",                   &battle_config.item_rate_common_boss	},	// [Reddozen]
	{ "item_rate_equip",                   &battle_config.item_rate_equip	},
	{ "item_rate_equip_boss",                   &battle_config.item_rate_equip_boss	},	// [Reddozen]
	{ "item_rate_card",                    &battle_config.item_rate_card	},	// End Addition
	{ "item_rate_card_boss",                    &battle_config.item_rate_card_boss	},	// [Reddozen]
	{ "item_rate_heal",                    &battle_config.item_rate_heal	},	// Added by Valaris
	{ "item_rate_heal_boss",                    &battle_config.item_rate_heal_boss	},	// [Reddozen]
	{ "item_rate_use",                     &battle_config.item_rate_use	},	// End
	{ "item_rate_use_boss",                     &battle_config.item_rate_use_boss	},	// [Reddozen]
	{ "item_rate_adddrop",                 &battle_config.item_rate_adddrop	},	// End
	{ "item_rate_treasure",                &battle_config.item_rate_treasure }, // End
	{ "day_duration",                      &battle_config.day_duration	}, // added by [Yor]
	{ "night_duration",                    &battle_config.night_duration	}, // added by [Yor]
	{ "max_heal",                          &battle_config.max_heal },
	{ "mob_remove_delay",                  &battle_config.mob_remove_delay	},
	{ "sg_miracle_skill_duration",         &battle_config.sg_miracle_skill_duration },
	{ "hvan_explosion_intimate",				&battle_config.hvan_explosion_intimate },	//[orn]
};

int battle_set_value(const char* w1, const char* w2) {
	int i;
	for(i = 0; i < sizeof(battle_data_short) / (sizeof(battle_data_short[0])); i++)
		if (strcmpi(w1, battle_data_short[i].str) == 0) {
			* battle_data_short[i].val = battle_config_switch(w2);
			return 1;
		}
	for(i = 0; i < sizeof(battle_data_int) / (sizeof(battle_data_int[0])); i++)
		if (strcmpi(w1, battle_data_int[i].str) == 0) {
			*battle_data_int[i].val = battle_config_switch(w2);
			return 1;
		}
	return 0;
}

int battle_get_value(const char* w1) {
	int i;
	for(i = 0; i < sizeof(battle_data_short) / (sizeof(battle_data_short[0])); i++)
		if (strcmpi(w1, battle_data_short[i].str) == 0) {
			return * battle_data_short[i].val;
		}
	for(i = 0; i < sizeof(battle_data_int) / (sizeof(battle_data_int[0])); i++)
		if (strcmpi(w1, battle_data_int[i].str) == 0) {
			return *battle_data_int[i].val;
		}
	return 0;
}

void battle_set_defaults() {
	battle_config.warp_point_debug=0;
	battle_config.enable_critical=BL_PC;
	battle_config.mob_critical_rate=100;
	battle_config.critical_rate=100;
	battle_config.enable_baseatk = BL_PC|BL_HOM;
	battle_config.enable_perfect_flee = BL_PC|BL_PET;
	battle_config.cast_rate=100;
	battle_config.delay_rate=100;
	battle_config.delay_dependon_agi=0;
	battle_config.sdelay_attack_enable=0;
	battle_config.left_cardfix_to_right=0;
	battle_config.skill_add_range=0;
	battle_config.skill_out_range_consume=1;
	battle_config.skillrange_by_distance=~BL_PC;
	battle_config.use_weapon_skill_range=~BL_PC;
	battle_config.pc_damage_delay_rate=100;
	battle_config.defnotenemy=0;
	battle_config.vs_traps_bctall=BL_PC;
	battle_config.traps_setting=0;
	battle_config.summon_flora=3;
	battle_config.clear_unit_ondeath=BL_ALL;
	battle_config.clear_unit_onwarp=BL_ALL;
	battle_config.random_monster_checklv=1;
	battle_config.attr_recover=1;
	battle_config.flooritem_lifetime=LIFETIME_FLOORITEM*1000;
	battle_config.item_auto_get=0;
	battle_config.item_first_get_time=3000;
	battle_config.item_second_get_time=1000;
	battle_config.item_third_get_time=1000;
	battle_config.mvp_item_first_get_time=10000;
	battle_config.mvp_item_second_get_time=10000;
	battle_config.mvp_item_third_get_time=2000;

	battle_config.drop_rate0item=0;
	battle_config.base_exp_rate=100;
	battle_config.job_exp_rate=100;
	battle_config.pvp_exp=1;
	battle_config.gtb_sc_immunity=50;
	battle_config.death_penalty_type=0;
	battle_config.death_penalty_base=0;
	battle_config.death_penalty_job=0;
	battle_config.zeny_penalty=0;
	battle_config.restart_hp_rate=0;
	battle_config.restart_sp_rate=0;
	battle_config.mvp_exp_rate=100;
	battle_config.mvp_hp_rate=100;
	battle_config.monster_hp_rate=100;
	battle_config.monster_max_aspd=199;
	battle_config.view_range_rate=100;
	battle_config.chase_range_rate=100;
	battle_config.atc_gmonly=0;
	battle_config.atc_spawn_quantity_limit=0;
	battle_config.atc_slave_clone_limit=0;
	battle_config.partial_name_scan=0;
	battle_config.gm_allskill=0;
	battle_config.gm_allequip=0;
	battle_config.gm_skilluncond=0;
	battle_config.gm_join_chat=0;
	battle_config.gm_kick_chat=0;
	battle_config.guild_max_castles=0;
	battle_config.emergency_call=15;
	battle_config.guild_aura=31;
	battle_config.skillfree = 0;
	battle_config.skillup_limit = 0;
	battle_config.wp_rate=100;
	battle_config.pp_rate=100;
	battle_config.monster_active_enable=1;
	battle_config.monster_damage_delay_rate=100;
	battle_config.monster_loot_type=0;
	battle_config.mob_skill_rate=100;
	battle_config.mob_skill_delay=100;
	battle_config.mob_count_rate=100;
	battle_config.mob_spawn_delay=100;
	battle_config.no_spawn_on_player=0;
	battle_config.force_random_spawn=0;
	battle_config.plant_spawn_delay=100;
	battle_config.boss_spawn_delay=100;
	battle_config.slaves_inherit_mode=2;
	battle_config.slaves_inherit_speed=3;
 	battle_config.summons_trigger_autospells=1; 
	battle_config.pc_walk_delay_rate=20;
	battle_config.walk_delay_rate=100;
	battle_config.multihit_delay=80;
	battle_config.quest_skill_learn=0;
	battle_config.quest_skill_reset=1;
	battle_config.basic_skill_check=1;
	battle_config.guild_emperium_check=1;
	battle_config.guild_exp_limit=50;
	battle_config.pc_invincible_time = 5000;
	battle_config.pet_catch_rate=100;
	battle_config.pet_rename=0;
	battle_config.pet_friendly_rate=100;
	battle_config.pet_hungry_delay_rate=100;
	battle_config.pet_hungry_friendly_decrease=5;
	battle_config.pet_status_support=0;
	battle_config.pet_attack_support=0;
	battle_config.pet_damage_support=0;
	battle_config.pet_support_min_friendly=900;
	battle_config.pet_support_rate=100;
	battle_config.pet_attack_exp_to_master=0;
	battle_config.pet_attack_exp_rate=100;
	battle_config.pet_lv_rate=0;	//Skotlex
	battle_config.pet_max_stats=99;	//Skotlex
	battle_config.pet_max_atk1=750;	//Skotlex
	battle_config.pet_max_atk2=1000;	//Skotlex
	battle_config.pet_no_gvg=0;	//Skotlex
	battle_config.skill_min_damage=6; //Ishizu claims that magic and misc attacks always do at least div_ damage. [Skotlex]
	battle_config.finger_offensive_type=0;
	battle_config.heal_exp=0;
	battle_config.max_heal=9999;
	battle_config.max_heal_lv=11;
	battle_config.resurrection_exp=0;
	battle_config.shop_exp=0;
	battle_config.combo_delay_rate=100;
	battle_config.item_check=1;
	battle_config.item_use_interval=100; //Use some very low value that won't bother players, but should cap bots.
	battle_config.wedding_modifydisplay=0;
	battle_config.wedding_ignorepalette=0;
	battle_config.xmas_ignorepalette=0; // [Valaris]
	battle_config.natural_healhp_interval=6000;
	battle_config.natural_healsp_interval=8000;
	battle_config.natural_heal_skill_interval=10000;
	battle_config.natural_heal_weight_rate=50;
	battle_config.arrow_decrement=1;
	battle_config.max_aspd = 199;
	battle_config.max_walk_speed = 300;
	battle_config.max_hp = 32500;
	battle_config.max_sp = 32500;
	battle_config.max_lv = 99; // [MouseJstr]
	battle_config.aura_lv = 99; // [Skotlex]
	battle_config.max_parameter = 99;
	battle_config.max_baby_parameter = 80;
	battle_config.max_cart_weight = 8000;
	battle_config.max_def = 99;	// [Skotlex]
	battle_config.over_def_bonus = 0;	// [Skotlex]
	battle_config.skill_log = 0;
	battle_config.battle_log = 0;
	battle_config.save_log = 0;
	battle_config.error_log = 1;
	battle_config.etc_log = 1;
	battle_config.save_clothcolor = 0;
	battle_config.undead_detect_type = 0;
	battle_config.auto_counter_type = BL_ALL;
	battle_config.min_hitrate = 5;
	battle_config.max_hitrate = 100;
	battle_config.agi_penalty_target = BL_PC;
	battle_config.agi_penalty_type = 1;
	battle_config.agi_penalty_count = 3;
	battle_config.agi_penalty_num = 10;
	battle_config.agi_penalty_count_lv = ATK_FLEE;
	battle_config.vit_penalty_target = BL_PC;
	battle_config.vit_penalty_type = 1;
	battle_config.vit_penalty_count = 3;
	battle_config.vit_penalty_num = 5;
	battle_config.vit_penalty_count_lv = ATK_DEF;
	battle_config.weapon_defense_type = 0;
	battle_config.magic_defense_type = 0;
	battle_config.skill_reiteration = 0;
	battle_config.skill_nofootset = BL_PC;
	battle_config.pc_cloak_check_type = 1;
	battle_config.monster_cloak_check_type = 0;
	battle_config.estimation_type = 3;
	battle_config.gvg_short_damage_rate = 80;
	battle_config.gvg_long_damage_rate = 80;
	battle_config.gvg_weapon_damage_rate = 60;
	battle_config.gvg_magic_damage_rate = 60;
	battle_config.gvg_misc_damage_rate = 60;
	battle_config.gvg_flee_penalty = 20;
	battle_config.gvg_eliminate_time = 7000;

	battle_config.pk_short_damage_rate = 80;
	battle_config.pk_long_damage_rate = 70;
	battle_config.pk_weapon_damage_rate = 60;
	battle_config.pk_magic_damage_rate = 60;
	battle_config.pk_misc_damage_rate = 60;

	battle_config.mob_changetarget_byskill = 0;
	battle_config.attack_direction_change = BL_ALL;
	battle_config.land_skill_limit = BL_ALL;
	battle_config.party_skill_penalty = 1;
	battle_config.monster_class_change_full_recover = 1;
	battle_config.produce_item_name_input = 0x3;
	battle_config.display_skill_fail = 0;
	battle_config.chat_warpportal = 0;
	battle_config.mob_warp = 0;
	battle_config.dead_branch_active = 0;
	battle_config.vending_max_value = 10000000;
	battle_config.show_steal_in_same_party = 0;
	battle_config.party_update_interval = 1000;
	battle_config.party_share_type = 0;
	battle_config.party_hp_mode = 0;
	battle_config.party_show_share_picker = 0;
	battle_config.attack_attr_none = ~BL_PC;
	battle_config.gx_allhit = 1;
	battle_config.gx_disptype = 1;
	battle_config.devotion_level_difference = 10;
	battle_config.player_skill_partner_check = 1;
	battle_config.hide_GM_session = 0;
	battle_config.invite_request_check = 1;
	battle_config.skill_removetrap_type = 0;
	battle_config.disp_experience = 0;
	battle_config.disp_zeny = 0;
	battle_config.castle_defense_rate = 100;
	battle_config.hp_rate = 100;
	battle_config.sp_rate = 100;
	battle_config.gm_cant_drop_min_lv = 1;
	battle_config.gm_cant_drop_max_lv = 0;
	battle_config.disp_hpmeter = 60;
	battle_config.skill_wall_check = 1;
	battle_config.cell_stack_limit = 1;
	battle_config.bone_drop = 0;
	battle_config.buyer_name = 1;

// eAthena additions
	battle_config.item_rate_mvp=100;
	battle_config.item_rate_common = 100;
	battle_config.item_rate_common_boss = 100;	// [Reddozen]
	battle_config.item_rate_equip = 100;
	battle_config.item_rate_equip_boss = 100;	// [Reddozen]
	battle_config.item_rate_card = 100;
	battle_config.item_rate_card_boss = 100;	// [Reddozen]
	battle_config.item_rate_heal = 100;		// Added by Valaris
	battle_config.item_rate_heal_boss = 100;	// [Reddozen]
	battle_config.item_rate_use = 100;		// End
	battle_config.item_rate_use_boss = 100;	// [Reddozen]
	battle_config.item_rate_adddrop = 100;
	battle_config.item_rate_treasure = 100;
	battle_config.logarithmic_drops = 0;
	battle_config.item_drop_common_min=1;	// Added by TyrNemesis^
	battle_config.item_drop_common_max=10000;
	battle_config.item_drop_equip_min=1;
	battle_config.item_drop_equip_max=10000;
	battle_config.item_drop_card_min=1;
	battle_config.item_drop_card_max=10000;
	battle_config.item_drop_mvp_min=1;
	battle_config.item_drop_mvp_max=10000;	// End Addition
	battle_config.item_drop_heal_min=1;		// Added by Valaris
	battle_config.item_drop_heal_max=10000;
	battle_config.item_drop_use_min=1;
	battle_config.item_drop_use_max=10000;	// End
	battle_config.item_drop_adddrop_min=1;
	battle_config.item_drop_adddrop_max=10000;
	battle_config.item_drop_treasure_min=1;
	battle_config.item_drop_treasure_max=10000;
	battle_config.prevent_logout = 10000;	// Added by RoVeRT
	battle_config.drops_by_luk = 0;	// [Valaris]
	battle_config.drops_by_luk2 = 0;
	battle_config.equip_natural_break_rate = 1;
	battle_config.equip_self_break_rate = 100; // [Valaris], adapted by [Skotlex]
	battle_config.equip_skill_break_rate = 100; // [Valaris], adapted by [Skotlex]
	battle_config.pk_mode = 0; // [Valaris]
	battle_config.pk_level_range = 0; // [Skotlex]
	battle_config.manner_system = 0xFFF; // [Valaris]
	battle_config.pet_equip_required = 0; // [Valaris]
	battle_config.multi_level_up = 0; // [Valaris]
	battle_config.max_exp_gain_rate	= 0; // [Skotlex]
	battle_config.backstab_bow_penalty = 0; // Akaru
	battle_config.night_at_start = 0; // added by [Yor]
	battle_config.day_duration = 2*60*60*1000; // added by [Yor] (2 hours)
	battle_config.night_duration = 30*60*1000; // added by [Yor] (30 minutes)
	battle_config.show_mob_info = 0;
	battle_config.hack_info_GM_level = 60; // added by [Yor] (default: 60, GM level)
	battle_config.any_warp_GM_min_level = 20; // added by [Yor]
	battle_config.packet_ver_flag = 1023; // added by [Yor]
	battle_config.min_hair_style = 0;
	battle_config.max_hair_style = 23;
	battle_config.min_hair_color = 0;
	battle_config.max_hair_color = 9;
	battle_config.min_cloth_color = 0;
	battle_config.max_cloth_color = 4;
	battle_config.pet_hair_style = 100;
	battle_config.zeny_from_mobs = 0;
	battle_config.mobs_level_up = 0; // [Valaris]
	battle_config.mobs_level_up_exp_rate = 1; // [Valaris]
	battle_config.pk_min_level = 55;
	battle_config.skill_steal_type = 1;
	battle_config.skill_steal_rate = 100;
	battle_config.skill_steal_max_tries = 0; //Default: unlimited tries.
	battle_config.motd_type = 0;
	battle_config.finding_ore_rate = 100;
	battle_config.castrate_dex_scale = 150;
	battle_config.area_size = 14;
	battle_config.exp_calc_type = 1;
	battle_config.exp_bonus_attacker = 25;
	battle_config.exp_bonus_max_attacker = 12;
	battle_config.min_skill_delay_limit = 100;
	battle_config.default_skill_delay = 300; //Default skill delay according to official servers.
	battle_config.no_skill_delay = BL_MOB;
	battle_config.attack_walk_delay = 0;
	battle_config.require_glory_guild = 0;
	battle_config.idle_no_share = 0;
	battle_config.party_even_share_bonus = 0;
	battle_config.delay_battle_damage = 1;
	battle_config.hide_woe_damage = 0;
	battle_config.display_version = 1;
	battle_config.display_site = 1;
	battle_config.who_display_aid = 0;
	battle_config.display_hallucination = 1;
	battle_config.ignore_items_gender = 1;
	battle_config.copyskill_restrict = 2;
	battle_config.berserk_cancels_buffs = 1;
	battle_config.debuff_on_logout = 1;
	battle_config.use_statpoint_table = 1;
	battle_config.mob_ai = 0;
	battle_config.hom_setting = 0xFFFF;
	battle_config.dynamic_mobs = 1; // use Dynamic Mobs [Wizputer]
	battle_config.mob_remove_damaged = 1; // Dynamic Mobs - Remove mobs even if damaged [Wizputer]
	battle_config.mob_remove_delay = 60000;
	battle_config.show_hp_sp_drain = 0; //Display drained hp/sp from attacks
	battle_config.show_hp_sp_gain = 1;	//Display gained hp/sp from mob-kills
	battle_config.mob_npc_event_type = 1; //Execute npc-event on player that delivered final blow.
	battle_config.mob_clear_delay = 0;
	battle_config.character_size = 3; //3: Peco riders Size=2, Baby Class Riders Size=1 [Lupus]
	battle_config.mob_max_skilllvl = MAX_SKILL_LEVEL; //max possible level of monsters skills [Lupus]
	battle_config.retaliate_to_master = 1; //Make mobs retaliate against the master rather than the mob that attacked them. [Skotlex]
	battle_config.rare_drop_announce = 1; //show global announces for rare items drops (<= 0.01% chance) [Lupus]
	battle_config.firewall_hits_on_undead = 1;
	battle_config.title_lvl1 = 1;	//Players Titles for @who, etc commands [Lupus]
	battle_config.title_lvl2 = 10;
	battle_config.title_lvl3 = 20;
	battle_config.title_lvl4 = 40;
	battle_config.title_lvl5 = 50;
	battle_config.title_lvl6 = 60;
	battle_config.title_lvl7 = 80;
	battle_config.title_lvl8 = 99;
	
	battle_config.duel_enable = 1;
	battle_config.duel_allow_pvp = 0;
	battle_config.duel_allow_gvg = 0;
	battle_config.duel_allow_teleport = 0;
	battle_config.duel_autoleave_when_die = 1;
	battle_config.duel_time_interval = 60;
	battle_config.duel_only_on_same_map = 0;
	
	battle_config.skip_teleport_lv1_menu = 0;
	battle_config.allow_skill_without_day = 0;
	battle_config.allow_es_magic_pc = 0;

	battle_config.skill_caster_check = 1;
	battle_config.sc_castcancel = BL_NUL;
	battle_config.pc_sc_def_rate = 100;
	battle_config.mob_sc_def_rate = 100;
	battle_config.pc_luk_sc_def = 300;
	battle_config.mob_luk_sc_def = 300;
	battle_config.pc_max_sc_def = 10000;
	battle_config.mob_max_sc_def = 5000;
	battle_config.sg_miracle_skill_ratio=1;
	battle_config.sg_angel_skill_ratio=1;
	battle_config.sg_miracle_skill_duration=3600000;
	battle_config.autospell_stacking = 0;
	battle_config.override_mob_names = 0;
	battle_config.min_chat_delay = 0;
	battle_config.block_area=0; // [Kamper]
	battle_config.friend_auto_add = 1;
	battle_config.hvan_explosion_intimate = 45000;	//[orn]
	battle_config.hom_rename=0;
	battle_config.homunculus_show_growth = 0;	//[orn]
	battle_config.homunculus_friendly_rate = 100;
}

void battle_validate_conf() {
	if(battle_config.flooritem_lifetime < 1000)
		battle_config.flooritem_lifetime = LIFETIME_FLOORITEM*1000;
/*	if(battle_config.restart_hp_rate < 0)
		battle_config.restart_hp_rate = 0;
	else*/ if(battle_config.restart_hp_rate > 100)
		battle_config.restart_hp_rate = 100;
/*	if(battle_config.restart_sp_rate < 0)
		battle_config.restart_sp_rate = 0;
	else*/ if(battle_config.restart_sp_rate > 100)
		battle_config.restart_sp_rate = 100;
	if(battle_config.natural_healhp_interval < NATURAL_HEAL_INTERVAL)
		battle_config.natural_healhp_interval=NATURAL_HEAL_INTERVAL;
	if(battle_config.natural_healsp_interval < NATURAL_HEAL_INTERVAL)
		battle_config.natural_healsp_interval=NATURAL_HEAL_INTERVAL;
	if(battle_config.natural_heal_skill_interval < NATURAL_HEAL_INTERVAL)
		battle_config.natural_heal_skill_interval=NATURAL_HEAL_INTERVAL;
	if(battle_config.natural_heal_weight_rate < 50)
		battle_config.natural_heal_weight_rate = 50;
	if(battle_config.natural_heal_weight_rate > 101)
		battle_config.natural_heal_weight_rate = 101;
	battle_config.monster_max_aspd = 2000 - battle_config.monster_max_aspd*10;
	if(battle_config.monster_max_aspd < 10)
		battle_config.monster_max_aspd = 10;
	if(battle_config.monster_max_aspd > 1000)
		battle_config.monster_max_aspd = 1000;
	battle_config.max_aspd = 2000 - battle_config.max_aspd*10;
	if(battle_config.max_aspd < 10)
		battle_config.max_aspd = 10;
	if(battle_config.max_aspd > 1000)
		battle_config.max_aspd = 1000;
	
	if (battle_config.max_walk_speed < 100)
		battle_config.max_walk_speed = 100;
	battle_config.max_walk_speed = 100*DEFAULT_WALK_SPEED/battle_config.max_walk_speed;
	if (battle_config.max_walk_speed < 1)
		battle_config.max_walk_speed = 1;
	
	if(battle_config.hp_rate < 1)
		battle_config.hp_rate = 1;
	if(battle_config.sp_rate < 1)
		battle_config.sp_rate = 1;
	if(battle_config.max_hp > 1000000000)
		battle_config.max_hp = 1000000000;
	if(battle_config.max_hp < 100)
		battle_config.max_hp = 100;
	if(battle_config.max_sp > 1000000000)
		battle_config.max_sp = 1000000000;
	if(battle_config.max_sp < 100)
		battle_config.max_sp = 100;
	if(battle_config.max_parameter < 10)
		battle_config.max_parameter = 10;
	if(battle_config.max_parameter > 10000)
		battle_config.max_parameter = 10000;
	if(battle_config.max_baby_parameter < 10)
		battle_config.max_baby_parameter = 10;
	if(battle_config.max_baby_parameter > 10000)
		battle_config.max_baby_parameter = 10000;
	if(battle_config.max_cart_weight > 1000000)
		battle_config.max_cart_weight = 1000000;
	if(battle_config.max_cart_weight < 100)
		battle_config.max_cart_weight = 100;
	battle_config.max_cart_weight *= 10;
	
	if(battle_config.max_def > 100 && !battle_config.weapon_defense_type)	 // added by [Skotlex]
		battle_config.max_def = 100;
	if(battle_config.over_def_bonus > 1000)
		battle_config.over_def_bonus = 1000;

	if(battle_config.min_hitrate > battle_config.max_hitrate)
		battle_config.min_hitrate = battle_config.max_hitrate;
		
	if(battle_config.agi_penalty_count < 2)
		battle_config.agi_penalty_count = 2;
	if(battle_config.vit_penalty_count < 2)
		battle_config.vit_penalty_count = 2;

	if(battle_config.party_update_interval < 100)
		battle_config.party_update_interval = 100;
	
	if(battle_config.guild_exp_limit > 99)
		battle_config.guild_exp_limit = 99;
/*	if(battle_config.guild_exp_limit < 0)
		battle_config.guild_exp_limit = 0;*/
	
	if(battle_config.pet_support_min_friendly > 950) //Capped to 950/1000 [Skotlex]
		battle_config.pet_support_min_friendly = 950;
	
	if(battle_config.pet_hungry_delay_rate < 10)
		battle_config.pet_hungry_delay_rate=10;
	
	if(battle_config.pet_max_atk1 > battle_config.pet_max_atk2)	//Skotlex
		battle_config.pet_max_atk1 = battle_config.pet_max_atk2;
	
//	if(battle_config.castle_defense_rate < 0)
//		battle_config.castle_defense_rate = 0;
	if(battle_config.castle_defense_rate > 100)
		battle_config.castle_defense_rate = 100;
	if(battle_config.item_drop_common_min < 1)		// Added by TyrNemesis^
		battle_config.item_drop_common_min = 1;
	if(battle_config.item_drop_common_max > 10000)
		battle_config.item_drop_common_max = 10000;
	if(battle_config.item_drop_equip_min < 1)
		battle_config.item_drop_equip_min = 1;
	if(battle_config.item_drop_equip_max > 10000)
		battle_config.item_drop_equip_max = 10000;
	if(battle_config.item_drop_card_min < 1)
		battle_config.item_drop_card_min = 1;
	if(battle_config.item_drop_card_max > 10000)
		battle_config.item_drop_card_max = 10000;
	if(battle_config.item_drop_mvp_min < 1)
		battle_config.item_drop_mvp_min = 1;
	if(battle_config.item_drop_mvp_max > 10000)
		battle_config.item_drop_mvp_max = 10000;	// End Addition

/*	if (battle_config.night_at_start < 0) // added by [Yor]
		battle_config.night_at_start = 0;
	else if (battle_config.night_at_start > 1) // added by [Yor]
		battle_config.night_at_start = 1; */
	if (battle_config.day_duration != 0 && battle_config.day_duration < 60000) // added by [Yor]
		battle_config.day_duration = 60000;
	if (battle_config.night_duration != 0 && battle_config.night_duration < 60000) // added by [Yor]
		battle_config.night_duration = 60000;
	
	if (battle_config.hack_info_GM_level > 100)
		battle_config.hack_info_GM_level = 100;

	if (battle_config.any_warp_GM_min_level > 100)
		battle_config.any_warp_GM_min_level = 100;

	if (battle_config.vending_max_value > MAX_ZENY || battle_config.vending_max_value==0)
		battle_config.vending_max_value = MAX_ZENY;

	if (battle_config.min_skill_delay_limit < 10)
		battle_config.min_skill_delay_limit = 10;	// minimum delay of 10ms

	if (battle_config.exp_bonus_max_attacker < 2)
		battle_config.exp_bonus_max_attacker = 2;

	if (battle_config.no_spawn_on_player > 100)
		battle_config.no_spawn_on_player = 100;
	if (battle_config.mob_remove_delay < 15000)	//Min 15 sec
		battle_config.mob_remove_delay = 15000;
	if (battle_config.dynamic_mobs > 1)
		battle_config.dynamic_mobs = 1;	//The flag will be used in assignations
	if (battle_config.mob_max_skilllvl> MAX_SKILL_LEVEL || battle_config.mob_max_skilllvl<1 )
		battle_config.mob_max_skilllvl = MAX_SKILL_LEVEL;

	if (battle_config.firewall_hits_on_undead < 1)
		battle_config.firewall_hits_on_undead = 1;
	else if (battle_config.firewall_hits_on_undead > 255) //The flag passed to battle_calc_damage is limited to 0xff
		battle_config.firewall_hits_on_undead = 255;

	if (battle_config.prevent_logout > 60000)
		battle_config.prevent_logout = 60000;

	if (battle_config.mobs_level_up_exp_rate < 1) // [Valaris]
		battle_config.mobs_level_up_exp_rate = 1;

	if (battle_config.pc_luk_sc_def < 1)
		battle_config.pc_luk_sc_def = 1;
	if (battle_config.mob_luk_sc_def < 1)
		battle_config.mob_luk_sc_def = 1;

	if (battle_config.sg_miracle_skill_ratio > 10000)
		battle_config.sg_miracle_skill_ratio = 10000;
	
	if (battle_config.skill_steal_max_tries >= UCHAR_MAX)
		battle_config.skill_steal_max_tries = UCHAR_MAX;	

#ifdef CELL_NOSTACK
	if (battle_config.cell_stack_limit < 1)
	  	battle_config.cell_stack_limit = 1;
	else 
	if (battle_config.cell_stack_limit > 255)
	  	battle_config.cell_stack_limit = 255;
#else 
	if (battle_config.cell_stack_limit != 1)
		ShowWarning("Battle setting 'cell_stack_limit' takes no effect as this server was compiled without Cell Stack Limit support.\n");
#endif

	if(battle_config.hvan_explosion_intimate > 100000)	//[orn]
		battle_config.hvan_explosion_intimate = 100000;
}

/*==========================================
 * ?έθt@CπΗέ?ή
 *------------------------------------------
 */
int battle_config_read(const char *cfgName)
{
	char line[1024], w1[1024], w2[1024];
	FILE *fp;
	static int count = 0;

	if ((count++) == 0)
		battle_set_defaults();

	fp = fopen(cfgName,"r");
	if (fp == NULL) {
		ShowError("File not found: %s\n", cfgName);
		return 1;
	}
	while(fgets(line,1020,fp)){
		if (line[0] == '/' && line[1] == '/')
			continue;
		if (sscanf(line, "%[^:]:%s", w1, w2) != 2)
			continue;
		if (strcmpi(w1, "import") == 0)
			battle_config_read(w2);
		else
			battle_set_value(w1, w2);
	}
	fclose(fp);

	if (--count == 0) {
		battle_validate_conf();
		add_timer_func_list(battle_delay_damage_sub, "battle_delay_damage_sub");
	}

	return 0;
}

void do_init_battle(void) {
	delay_damage_ers = ers_new(sizeof(struct delay_damage));
}

void do_final_battle(void) {
	ers_destroy(delay_damage_ers);
}
