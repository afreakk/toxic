/*  groupchat.c
 *
 *
 *  Copyright (C) 2014 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE    /* needed for strcasestr() and wcswidth() */
#endif

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <wchar.h>
#include <unistd.h>

#ifdef AUDIO
#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
/* compatibility with older versions of OpenAL */
#ifndef ALC_ALL_DEVICES_SPECIFIER
#include <AL/alext.h>
#endif  /* ALC_ALL_DEVICES_SPECIFIER */
#endif  /* __APPLE__ */
#endif  /* AUDIO */

#include "windows.h"
#include "toxic.h"
#include "execute.h"
#include "misc_tools.h"
#include "groupchat.h"
#include "prompt.h"
#include "toxic_strings.h"
#include "log.h"
#include "line_info.h"
#include "settings.h"
#include "input.h"
#include "help.h"
#include "notify.h"
#include "autocomplete.h"
#include "device.h"

extern char *DATA_FILE;

static GroupChat groupchats[MAX_GROUPCHAT_NUM];
static int max_groupchat_index = 0;

extern struct user_settings *user_settings;
extern struct Winthread Winthread;

#ifdef AUDIO
#define AC_NUM_GROUP_COMMANDS 38
#else
#define AC_NUM_GROUP_COMMANDS 34
#endif /* AUDIO */

/* groupchat command names used for tab completion. */
static const char group_cmd_list[AC_NUM_GROUP_COMMANDS][MAX_CMDNAME_SIZE] = {
    { "/accept"     },
    { "/add"        },
    { "/avatar"     },
    { "/ban"        },
    { "/chatid"     },
    { "/clear"      },
    { "/close"      },
    { "/connect"    },
    { "/decline"    },
    { "/exit"       },
    { "/group"      },
    { "/help"       },
    { "/ignore"     },
    { "/join"       },
    { "/kick"       },
    { "/log"        },
    { "/mod"        },
    { "/myid"       },
    { "/nick"       },
    { "/note"       },
    { "/passwd"     },
    { "/peerlimit"  },
    { "/privacy"    },
    { "/quit"       },
    { "/rejoin"     },
    { "/requests"   },
    { "/silence"    },
    { "/status"     },
    { "/topic"      },
    { "/unban"      },
    { "/unignore"   },
    { "/unmod"      },
    { "/unsilence"  },
    { "/whisper"    },

#ifdef AUDIO

    { "/lsdev"       },
    { "/sdev"        },
    { "/mute"        },
    { "/sense"       },

#endif /* AUDIO */
};

static void groupchat_set_group_name(ToxWindow *self, Tox *m, uint32_t groupnum);
ToxWindow new_group_chat(Tox *m, uint32_t groupnum, const char *groupname, int length);
static void groupchat_onGroupPeerlistUpdate(ToxWindow *self, Tox *m, uint32_t groupnum);

int init_groupchat_win(Tox *m, uint32_t groupnum, const char *groupname, size_t length)
{
    if (groupnum > MAX_GROUPCHAT_NUM)
        return -1;

    ToxWindow self = new_group_chat(m, groupnum, groupname, length);

    /* In case we're loading a saved group */
    if (length == 0)
        groupchat_set_group_name(&self, m, groupnum);

    int i;

    for (i = 0; i <= max_groupchat_index; ++i) {
        if (!groupchats[i].active) {
            groupchats[i].chatwin = add_window(m, self);
            groupchats[i].active = true;
            groupchats[i].groupnumber = groupnum;

            if (i == max_groupchat_index)
                ++max_groupchat_index;

            set_active_window(groupchats[i].chatwin);
            groupchat_onGroupPeerlistUpdate(&self, m, groupnum);
            store_data(m, DATA_FILE);

            return 0;
        }
    }

    return -1;
}

static void kill_groupchat_window(ToxWindow *self)
{
    ChatContext *ctx = self->chatwin;

    log_disable(ctx->log);
    line_info_cleanup(ctx->hst);
    delwin(ctx->linewin);
    delwin(ctx->history);
    delwin(ctx->sidebar);
    free(ctx->log);
    free(ctx);
    free(self->help);
    del_window(self);
}

/* Closes groupchat window and cleans up. */
void close_groupchat(ToxWindow *self, Tox *m, uint32_t groupnum)
{
    GroupChat *chat = &groupchats[groupnum];

    if (chat->peer_list)
        free(chat->peer_list);

    if (chat->name_list)
        free(chat->name_list);

    memset(&groupchats[groupnum], 0, sizeof(GroupChat));

    int i;

    for (i = max_groupchat_index; i > 0; --i) {
        if (groupchats[i - 1].active)
            break;
    }

    max_groupchat_index = i;
    kill_groupchat_window(self);
}

static void exit_groupchat(ToxWindow *self, Tox *m, uint32_t groupnum, const char *partmessage, size_t length)
{
    if (length > TOX_GROUP_MAX_PART_LENGTH)
        length = TOX_GROUP_MAX_PART_LENGTH;

    tox_group_leave(m, groupnum, (uint8_t *) partmessage, length, NULL);
    close_groupchat(self, m, groupnum);
}

/* Note: the arguments to these functions are validated in the caller functions */
void set_nick_all_groups(Tox *m, const char *nick, size_t length)
{
    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    int i;

    for (i = 0; i < max_groupchat_index; ++i) {
        if (groupchats[i].active) {
            ToxWindow *self = get_window_ptr(groupchats[i].chatwin);

            TOX_ERR_GROUP_SELF_NAME_SET err;
            tox_group_self_set_name(m, groupchats[i].groupnumber, (uint8_t *) nick, length, &err);

            switch (err) {
                case TOX_ERR_GROUP_SELF_NAME_SET_OK: {
                    break;
                }
                case TOX_ERR_GROUP_SELF_NAME_SET_TAKEN: {
                    line_info_add(self, NULL, NULL, 0, SYS_MSG, 0, RED, "-!- That nick is already in use.");
                    break;
                }
                default: {
                    if (groupchats[i].is_connected)
                        line_info_add(self, NULL, NULL, 0, SYS_MSG, 0, RED, "-!- Failed to set nick (error %d).", err);

                    break;
                }
            }
        }
    }
}

void set_status_all_groups(Tox *m, uint8_t status)
{
    int i;

    for (i = 0; i < max_groupchat_index; ++i) {
        if (groupchats[i].active)
            tox_group_self_set_status(m, groupchats[i].groupnumber, (TOX_USER_STATUS) status, NULL);
    }
}

int group_get_nick_peernumber(uint32_t groupnum, const char *nick)
{
    uint32_t i;

    for (i = 0; i < groupchats[groupnum].num_peers; ++i) {
        if (strcmp(nick, groupchats[groupnum].peer_list[i].name) == 0)
            return i;
    }

    return -1;
}

static void group_update_name_list(uint32_t groupnum)
{
    GroupChat *chat = &groupchats[groupnum];

    if (chat->name_list)
        free(chat->name_list);

    chat->name_list = malloc(sizeof(char *) * chat->num_peers * TOX_MAX_NAME_LENGTH);

    if (chat->name_list == NULL)
        exit_toxic_err("failed in group_update_name_list", FATALERR_MEMORY);

    uint32_t i;

    for (i = 0; i < chat->num_peers; ++i)
        memcpy(&chat->name_list[i * TOX_MAX_NAME_LENGTH], chat->peer_list[i].name, chat->peer_list[i].name_length + 1);
}

/* destroys and re-creates groupchat window */
void redraw_groupchat_win(ToxWindow *self)
{
    ChatContext *ctx = self->chatwin;

    endwin();
    refresh();
    clear();

    int x2, y2;
    getmaxyx(stdscr, y2, x2);
    y2 -= 2;

    if (ctx->sidebar) {
        delwin(ctx->sidebar);
        ctx->sidebar = NULL;
    }

    delwin(ctx->linewin);
    delwin(ctx->history);
    delwin(self->window);

    self->window = newwin(y2, x2, 0, 0);
    ctx->linewin = subwin(self->window, CHATBOX_HEIGHT, x2, y2 - CHATBOX_HEIGHT, 0);

    if (self->show_peerlist) {
        ctx->history = subwin(self->window, y2 - CHATBOX_HEIGHT + 1, x2 - SIDEBAR_WIDTH - 1, 0, 0);
        ctx->sidebar = subwin(self->window, y2 - CHATBOX_HEIGHT + 1, SIDEBAR_WIDTH, 0, x2 - SIDEBAR_WIDTH);
    } else {
        ctx->history = subwin(self->window, y2 - CHATBOX_HEIGHT + 1, x2, 0, 0);
    }

    scrollok(ctx->history, 0);

}

static void group_onAction(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum, const char *action,
                           size_t len)
{
    ChatContext *ctx = self->chatwin;

    char nick[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, nick, peernum, groupnum);

    char selfnick[TOX_MAX_NAME_LENGTH];
    tox_self_get_name(m, (uint8_t *) selfnick);

    size_t n_len = tox_self_get_name_size(m);
    selfnick[n_len] = '\0';

    if (strcasestr(action, selfnick)) {
        sound_notify(self, generic_message, NT_WNDALERT_0, NULL);

        if (self->active_box != -1)
            box_silent_notify2(self, NT_NOFOCUS, self->active_box, "* %s %s", nick, action );
        else
            box_silent_notify(self, NT_NOFOCUS, &self->active_box, self->name, "* %s %s", nick, action);
    } else {
        sound_notify(self, silent, NT_WNDALERT_1, NULL);
    }

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, nick, NULL, IN_ACTION, 0, 0, "%s", action);
    write_to_log(action, nick, ctx->log, true);
}

static void groupchat_onGroupMessage(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum,
                                     TOX_MESSAGE_TYPE type, const char *msg, size_t len)
{
    if (self->num != groupnum)
        return;

    if (type == TOX_MESSAGE_TYPE_ACTION) {
        group_onAction(self, m, groupnum, peernum, msg, len);
        return;
    }

    ChatContext *ctx = self->chatwin;

    char nick[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, nick, peernum, groupnum);

    char selfnick[TOX_MAX_NAME_LENGTH];
    tox_self_get_name(m, (uint8_t *) selfnick);

    size_t sn_len = tox_self_get_name_size(m);
    selfnick[sn_len] = '\0';

    int nick_clr = CYAN;

    /* Only play sound if mentioned by someone else */
    if (strcasestr(msg, selfnick) && strcmp(selfnick, nick)) {
        sound_notify(self, generic_message, NT_WNDALERT_0, NULL);

        if (self->active_box != -1)
            box_silent_notify2(self, NT_NOFOCUS, self->active_box, "%s %s", nick, msg);
        else
            box_silent_notify(self, NT_NOFOCUS, &self->active_box, self->name, "%s %s", nick, msg);

        nick_clr = RED;
    } else {
        sound_notify(self, silent, NT_WNDALERT_1, NULL);
    }

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, nick, NULL, IN_MSG, 0, nick_clr, "%s", msg);
    write_to_log(msg, nick, ctx->log, false);
}

static void groupchat_onGroupPrivateMessage(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum,
                                            const char *msg, size_t len)
{
    if (self->num != groupnum)
        return;

    ChatContext *ctx = self->chatwin;

    char nick[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, nick, peernum, groupnum);

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, nick, NULL, IN_PRVT_MSG, 0, MAGENTA, "%s", msg);
    write_to_log(msg, nick, ctx->log, false);

    sound_notify(self, generic_message, NT_WNDALERT_0, NULL);

    if (self->active_box != -1)
        box_silent_notify2(self, NT_NOFOCUS, self->active_box, "%s %s", nick, msg);
    else
        box_silent_notify(self, NT_NOFOCUS, &self->active_box, self->name, "%s %s", nick, msg);
}

static void groupchat_onGroupTopicChange(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum,
                                         const char *topic, size_t length)
{
    ChatContext *ctx = self->chatwin;

    if (self->num != groupnum)
        return;

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    char nick[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, nick, peernum, groupnum);
    line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, MAGENTA, "-!- %s set the topic to: %s", nick, topic);

    char tmp_event[MAX_STR_SIZE];
    snprintf(tmp_event, sizeof(tmp_event), " set the topic to %s", topic);
    write_to_log(tmp_event, nick, ctx->log, true);
}

static void groupchat_onGroupPeerLimit(ToxWindow *self, Tox *m, uint32_t groupnumber, uint32_t peer_limit)
{
    ChatContext *ctx = self->chatwin;

    if (self->num != groupnumber)
        return;

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- The group founder has set the peer limit to %d", peer_limit);

    char tmp_event[MAX_STR_SIZE];
    snprintf(tmp_event, sizeof(tmp_event), " set the peer limit to %d", peer_limit);
    write_to_log(tmp_event, "The founder", ctx->log, true);
}

static void groupchat_onGroupPrivacyState(ToxWindow *self, Tox *m, uint32_t groupnumber, TOX_GROUP_PRIVACY_STATE state)
{
    ChatContext *ctx = self->chatwin;

    if (self->num != groupnumber)
        return;

    const char *state_str = state == TOX_GROUP_PRIVACY_STATE_PUBLIC ? "public" : "private";

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- The group founder has set the group to %s.", state_str);

    char tmp_event[MAX_STR_SIZE];
    snprintf(tmp_event, sizeof(tmp_event), " set the group to %s.", state_str);
    write_to_log(tmp_event, "The founder", ctx->log, true);
}

static void groupchat_onGroupPassword(ToxWindow *self, Tox *m, uint32_t groupnumber, const char *password,
                                      size_t length)
{
    ChatContext *ctx = self->chatwin;

    if (self->num != groupnumber)
        return;

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    if (length > 0) {
        line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- The group founder has password protected the group.");

        char tmp_event[MAX_STR_SIZE];
        snprintf(tmp_event, sizeof(tmp_event), " set a new password.");
        write_to_log(tmp_event, "The founder", ctx->log, true);
    } else {
        line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- The group founder has removed password protection.");

        char tmp_event[MAX_STR_SIZE];
        snprintf(tmp_event, sizeof(tmp_event), " removed password protection.");
        write_to_log(tmp_event, "The founder", ctx->log, true);
    }
}

/* TODO: This function could probably be cleaned up and optimized */
static void groupchat_onGroupPeerlistUpdate(ToxWindow *self, Tox *m, uint32_t groupnum)
{
    if (self->num != groupnum)
        return;

    TOX_ERR_GROUP_STATE_QUERIES err;
    uint32_t num_peers = tox_group_get_number_peers(m, groupnum, &err);

    if (num_peers == 0 || err != TOX_ERR_GROUP_STATE_QUERIES_OK)
        return;

    GroupChat *chat = &groupchats[groupnum];
    chat->num_peers = num_peers;

    if (chat->peer_list)
        free(chat->peer_list);

    chat->peer_list = malloc(sizeof(struct GroupPeer) * num_peers);

    if (chat->peer_list == NULL)
        exit_toxic_err("failed in groupchat_onGroupPeerlistUpdate", FATALERR_MEMORY);

    uint32_t i;

    for (i = 0; i < num_peers; ++i) {
        memset(&chat->peer_list[i], 0, sizeof(struct GroupPeer));

        TOX_ERR_GROUP_PEER_QUERY err;
        size_t name_length = tox_group_peer_get_name_size(m, groupnum, i, &err);

        if (err != TOX_ERR_GROUP_PEER_QUERY_OK)
            name_length = 0;
        else
            name_length = MIN(name_length, TOX_MAX_NAME_LENGTH - 1);

        if (!tox_group_peer_get_name(m, groupnum, i, (uint8_t *) chat->peer_list[i].name, NULL)) {
            chat->peer_list[i].name_length = strlen(UNKNOWN_NAME);
            memcpy(chat->peer_list[i].name, UNKNOWN_NAME, chat->peer_list[i].name_length);
        }

        chat->peer_list[i].name[name_length] = '\0';
        chat->peer_list[i].name_length = name_length;
        chat->peer_list[i].status = tox_group_peer_get_status(m, groupnum, i, NULL);
        chat->peer_list[i].role = tox_group_peer_get_role(m, groupnum, i, NULL);
    }

    group_update_name_list(groupnum);
}

static void groupchat_onGroupPeerJoin(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum)
{
    if (groupnum != self->num)
        return;

    if (peernum > groupchats[groupnum].num_peers)
        return;

    char name[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, name, peernum, groupnum);

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, name, NULL, CONNECTION, 0, GREEN, "has joined the room.");

    char log_str[TOXIC_MAX_NAME_LENGTH + 32];
    snprintf(log_str, sizeof(log_str), "%s has joined the room", name);

    write_to_log(log_str, name, self->chatwin->log, true);
    sound_notify(self, silent, NT_WNDALERT_2, NULL);
}

static void groupchat_onGroupPeerExit(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum,
                                      const char *partmessage, size_t len)
{
    if (groupnum != self->num)
        return;

    if (peernum > groupchats[groupnum].num_peers)
        return;

    char name[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, name, peernum, groupnum);

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, name, NULL, DISCONNECTION, 0, RED, "has left the room (%s)", partmessage);

    char log_str[TOXIC_MAX_NAME_LENGTH + MAX_STR_SIZE];
    snprintf(log_str, sizeof(log_str), "%s has left the room (%s)", name, partmessage);

    write_to_log(log_str, name, self->chatwin->log, true);
    sound_notify(self, silent, NT_WNDALERT_2, NULL);
}

static void groupchat_set_group_name(ToxWindow *self, Tox *m, uint32_t groupnum)
{
    TOX_ERR_GROUP_STATE_QUERIES err;
    size_t len = tox_group_get_name_size(m, groupnum, &err);

    if (err != TOX_ERR_GROUP_STATE_QUERIES_OK) {
        line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, 0, "Failed to retrieve group name length (error %d)", err);
        return;
    }

    char tmp_groupname[len];

    if (!tox_group_get_name(m, groupnum, (uint8_t *) tmp_groupname, &err)) {
        line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, 0, "Failed to retrieve group name (error %d)", err);
        return;
    }

    char groupname[len + 1];
    len = copy_tox_str(groupname, sizeof(groupname), tmp_groupname, len);

    if (len > 0)
        set_window_title(self, groupname, len);
}

static void groupchat_onGroupSelfJoin(ToxWindow *self, Tox *m, uint32_t groupnum)
{
    if (groupnum != self->num)
        return;

    groupchat_set_group_name(self, m, groupnum);

    int i;

    for (i = 0; i < max_groupchat_index; ++i) {
        if (groupchats[i].active && groupchats[i].groupnumber == groupnum) {
            groupchats[i].is_connected = true;
            break;
        }
    }

    TOX_ERR_GROUP_STATE_QUERIES err;
    size_t len = tox_group_get_topic_size(m, groupnum, &err);

    if (err != TOX_ERR_GROUP_STATE_QUERIES_OK) {
        line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, 0, "Failed to retrieve group topic length (error %d)", err);
        return;
    }

    char tmp_topic[len];
    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    if (tox_group_get_topic(m, groupnum, (uint8_t *) tmp_topic, &err)) {
        char topic[len + 1];
        copy_tox_str(topic, sizeof(topic), tmp_topic, len);
        line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, MAGENTA, "-!- Topic set to: %s", topic);
    } else {
        line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, 0, "Failed to retrieve group topic (error %d)", err);
    }
}

static void groupchat_onGroupRejected(ToxWindow *self, Tox *m, uint32_t groupnum, TOX_GROUP_JOIN_FAIL type)
{
    if (groupnum != self->num)
        return;

    const char *msg = NULL;

    switch (type) {
        case TOX_GROUP_JOIN_FAIL_NAME_TAKEN:
            msg = "Nick already in use. Change your nick and use the '/rejoin' command.";
            break;
        case TOX_GROUP_JOIN_FAIL_PEER_LIMIT:
            msg = "Group is full. Try again with the '/rejoin' command.";
            break;
        case TOX_GROUP_JOIN_FAIL_INVALID_PASSWORD:
            msg = "Invalid password.";
            break;
        case TOX_GROUP_JOIN_FAIL_UNKNOWN:
            msg = "Failed to join group. Try again with the '/rejoin' command.";
            break;
    }

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 0, RED, "-!- %s", msg);
}

static void groupchat_onGroupModeration(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t src_peernum,
                                        uint32_t tgt_peernum, TOX_GROUP_MOD_EVENT type)
{
    if (groupnum != self->num)
        return;

    GroupChat *chat = &groupchats[groupnum];

    char src_name[TOX_MAX_NAME_LENGTH];
    char tgt_name[TOX_MAX_NAME_LENGTH];

    if (get_group_nick_truncate(m, src_name, src_peernum, groupnum) == -1)
        return;

    if (get_group_nick_truncate(m, tgt_name, tgt_peernum, groupnum) == -1)
        return;

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    switch (type) {
        case TOX_GROUP_MOD_EVENT_KICK:
            line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, RED, "-!- %s has been kicked by %s", tgt_name, src_name);
            break;
        case TOX_GROUP_MOD_EVENT_BAN:
            line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, RED, "-!- %s has been banned by %s", tgt_name, src_name);
            break;
        case TOX_GROUP_MOD_EVENT_OBSERVER:
            chat->peer_list[tgt_peernum].role = TOX_GROUP_ROLE_OBSERVER;
            line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- %s has set %s's role to observer", src_name, tgt_name);
            break;
        case TOX_GROUP_MOD_EVENT_USER:
            chat->peer_list[tgt_peernum].role = TOX_GROUP_ROLE_USER;
            line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- %s has set %s's role to user", src_name, tgt_name);
            break;
        case TOX_GROUP_MOD_EVENT_MODERATOR:
            chat->peer_list[tgt_peernum].role = TOX_GROUP_ROLE_MODERATOR;
            line_info_add(self, timefrmt, NULL, NULL, SYS_MSG, 1, BLUE, "-!- %s has set %s's role to moderator", src_name, tgt_name);
            break;
        default:
            return;
    }
}

static void groupchat_onGroupNickChange(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum,
                                        const char *newnick, size_t len)
{
    if (groupnum != self->num)
        return;

    GroupChat *chat = &groupchats[groupnum];

    char oldnick[TOX_MAX_NAME_LENGTH];
    get_group_nick_truncate(m, oldnick, peernum, groupnum);

    len = MAX(len, TOX_MAX_NAME_LENGTH - 1);
    memcpy(groupchats[groupnum].peer_list[peernum].name, newnick, len);
    chat->peer_list[peernum].name[len] = '\0';
    chat->peer_list[peernum].name_length = len;

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));
    line_info_add(self, timefrmt, oldnick, chat->peer_list[peernum].name, NAME_CHANGE, 0, MAGENTA, " is now known as ");

    group_update_name_list(groupnum);
}

static void groupchat_onGroupStatusChange(ToxWindow *self, Tox *m, uint32_t groupnum, uint32_t peernum,
                                          TOX_USER_STATUS status)
{
    if (groupnum != self->num)
        return;

    GroupChat *chat = &groupchats[groupnum];
    chat->peer_list[peernum].status = status;
}

static void send_group_message(ToxWindow *self, Tox *m, uint32_t groupnum, const char *msg, TOX_MESSAGE_TYPE type)
{
    ChatContext *ctx = self->chatwin;

    if (msg == NULL) {
        wprintw(ctx->history, "Message is empty.\n");
        return;
    }

    TOX_ERR_GROUP_SEND_MESSAGE err;
    if (!tox_group_send_message(m, groupnum, type, (uint8_t *) msg, strlen(msg), &err)) {
        if (err == TOX_ERR_GROUP_SEND_MESSAGE_PERMISSIONS) {
            line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, RED, " * You are silenced.");
        } else {
            line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, RED, " * Failed to send message (Error %d).", err);
        }

        return;
    }

    TOX_ERR_GROUP_SELF_QUERY sn_err;
    size_t len = tox_group_self_get_name_size(m, groupnum, &sn_err);

    if (sn_err != TOX_ERR_GROUP_SELF_QUERY_OK)
        return;

    char selfname[len + 1];

    if (!tox_group_self_get_name(m, groupnum, (uint8_t *) selfname, NULL))
        return;

    selfname[len] = '\0';

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));


    if (type == TOX_MESSAGE_TYPE_NORMAL) {
        line_info_add(self, timefrmt, selfname, NULL, OUT_MSG_READ, 0, 0, "%s", msg);
        write_to_log(msg, selfname, ctx->log, false);
    } else if (type == TOX_MESSAGE_TYPE_ACTION) {
        line_info_add(self, timefrmt, selfname, NULL, OUT_ACTION_READ, 0, 0, "%s", msg);
        write_to_log(msg, selfname, ctx->log, true);
    }
}

static void send_group_prvt_message(ToxWindow *self, Tox *m, uint32_t groupnum, const char *data)
{
    ChatContext *ctx = self->chatwin;
    GroupChat *chat = &groupchats[groupnum];

    if (data == NULL) {
        wprintw(ctx->history, "Message is empty.\n");
        return;
    }

    size_t i;
    int peernum = -1, nick_len = 0;
    char *nick = NULL;

    /* need to match the longest nick in case of nicks that are smaller sub-strings */
    for (i = 0; i < chat->num_peers; ++i) {
        if (memcmp(chat->peer_list[i].name, data, chat->peer_list[i].name_length) == 0) {
            if (chat->peer_list[i].name_length > nick_len) {
                nick_len = chat->peer_list[i].name_length;
                nick = chat->peer_list[i].name;
                peernum = i;
            }
        }
    }

    if (peernum == -1) {
        line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, 0, "Invalid peer name.");
        return;
    }

    int msg_len = strlen(data) - chat->peer_list[i].name_length - 1;

    if (msg_len <= 0)
        return;

    const char *msg = data + chat->peer_list[i].name_length + 1;

    TOX_ERR_GROUP_SEND_PRIVATE_MESSAGE err;
    if (!tox_group_send_private_message(m, groupnum, peernum, (uint8_t *) msg, msg_len, &err)) {
        if (err == TOX_ERR_GROUP_SEND_PRIVATE_MESSAGE_PERMISSIONS) {
            line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, RED, " * You are silenced.");
        } else {
            line_info_add(self, NULL, NULL, NULL, SYS_MSG, 0, RED, " * Failed to send private message.");
        }

        return;
    }

    /* turn "peername" into ">peername<" to signify private message */
    char pm_nick[TOX_MAX_NAME_LENGTH + 2];
    strcpy(pm_nick, ">");
    strcpy(pm_nick + 1, nick);
    strcpy(pm_nick + 1 + chat->peer_list[i].name_length, "<");

    char timefrmt[TIME_STR_SIZE];
    get_time_str(timefrmt, sizeof(timefrmt));

    line_info_add(self, timefrmt, pm_nick, NULL, OUT_PRVT_MSG, 0, 0, "%s", msg);
    write_to_log(msg, pm_nick, ctx->log, false);
}

static void groupchat_onKey(ToxWindow *self, Tox *m, wint_t key, bool ltr)
{
    ChatContext *ctx = self->chatwin;
    GroupChat *chat = &groupchats[self->num];

    int x, y, y2, x2;
    getyx(self->window, y, x);
    getmaxyx(self->window, y2, x2);

    if (x2 <= 0)
        return;

    if (self->help->active) {
        help_onKey(self, key);
        return;
    }

    if (ltr) {    /* char is printable */
        input_new_char(self, key, x, y, x2, y2);
        return;
    }

    if (line_info_onKey(self, key))
        return;

    if (input_handle(self, key, x, y, x2, y2))
        return;

    if (key == '\t') {  /* TAB key: auto-completes peer name or command */
        if (ctx->len > 0) {
            int diff = -1;

            /* TODO: make this not suck */
            if (ctx->line[0] != L'/' || wcschr(ctx->line, L' ') != NULL) {
                diff = complete_line(self, chat->name_list, chat->num_peers, TOX_MAX_NAME_LENGTH);
            } else if (wcsncmp(ctx->line, L"/avatar \"", wcslen(L"/avatar \"")) == 0) {
                diff = dir_match(self, m, ctx->line, L"/avatar");
            } else {
                diff = complete_line(self, group_cmd_list, AC_NUM_GROUP_COMMANDS, MAX_CMDNAME_SIZE);
            }

            if (diff != -1) {
                if (x + diff > x2 - 1) {
                    int wlen = wcswidth(ctx->line, sizeof(ctx->line));
                    ctx->start = wlen < x2 ? 0 : wlen - x2 + 1;
                }
            } else {
                sound_notify(self, notif_error, 0, NULL);
            }
        } else {
            sound_notify(self, notif_error, 0, NULL);
        }
    } else if (key == user_settings->key_peer_list_down) {    /* Scroll peerlist up and down one position */
        int L = y2 - CHATBOX_HEIGHT - SDBAR_OFST;

        if (groupchats[self->num].side_pos < groupchats[self->num].num_peers - L)
            ++chat->side_pos;
    } else if (key == user_settings->key_peer_list_up) {
        if (groupchats[self->num].side_pos > 0)
            --chat->side_pos;
    } else if (key == '\n') {
        rm_trailing_spaces_buf(ctx);

        char line[MAX_STR_SIZE];

        if (wcs_to_mbs_buf(line, ctx->line, MAX_STR_SIZE) == -1)
            memset(&line, 0, sizeof(line));

        if (!string_is_empty(line))
            add_line_to_hist(ctx);

        if (line[0] == '/') {
            if (strncmp(line, "/close", strlen("/close")) == 0) {
                int offset = 6;

                if (line[offset] != '\0')
                    ++offset;

                exit_groupchat(self, m, self->num, line + offset, ctx->len - offset);
                return;
            } else if (strncmp(line, "/me ", strlen("/me ")) == 0) {
                send_group_message(self, m, self->num, line + 4, TOX_MESSAGE_TYPE_ACTION);
            } else if (strncmp(line, "/whisper ", strlen("/whisper ")) == 0) {
                send_group_prvt_message(self, m, self->num, line + 9);
            } else {
                execute(ctx->history, self, m, line, GROUPCHAT_COMMAND_MODE);
            }
        } else if (!string_is_empty(line)) {
            send_group_message(self, m, self->num, line, TOX_MESSAGE_TYPE_NORMAL);
        }

        wclear(ctx->linewin);
        wmove(self->window, y2 - CURS_Y_OFFSET, 0);
        reset_buf(ctx);
    }
}

static void groupchat_onDraw(ToxWindow *self, Tox *m)
{
    int x2, y2;
    getmaxyx(self->window, y2, x2);

    ChatContext *ctx = self->chatwin;
    GroupChat *chat = &groupchats[self->num];

    pthread_mutex_lock(&Winthread.lock);
    line_info_print(self);
    pthread_mutex_unlock(&Winthread.lock);

    wclear(ctx->linewin);

    curs_set(1);

    if (ctx->len > 0)
        mvwprintw(ctx->linewin, 1, 0, "%ls", &ctx->line[ctx->start]);

    wclear(ctx->sidebar);
    mvwhline(self->window, y2 - CHATBOX_HEIGHT, 0, ACS_HLINE, x2);

    if (self->show_peerlist) {
        mvwvline(ctx->sidebar, 0, 0, ACS_VLINE, y2 - CHATBOX_HEIGHT);
        mvwaddch(ctx->sidebar, y2 - CHATBOX_HEIGHT, 0, ACS_BTEE);

        pthread_mutex_lock(&Winthread.lock);

        wmove(ctx->sidebar, 0, 1);
        wattron(ctx->sidebar, A_BOLD);
        wprintw(ctx->sidebar, "Peers: %d\n", chat->num_peers);
        wattroff(ctx->sidebar, A_BOLD);

        mvwaddch(ctx->sidebar, 1, 0, ACS_LTEE);
        mvwhline(ctx->sidebar, 1, 1, ACS_HLINE, SIDEBAR_WIDTH - 1);

        int maxlines = y2 - SDBAR_OFST - CHATBOX_HEIGHT;
        uint32_t i;

        for (i = 0; i < chat->num_peers && i < maxlines; ++i) {
            wmove(ctx->sidebar, i + 2, 1);
            int peer = i + chat->side_pos;

            int maxlen_offset = chat->peer_list[i].role == TOX_GROUP_ROLE_USER ? 2 : 3;

            /* truncate nick to fit in side panel without modifying list */
            char tmpnck[TOX_MAX_NAME_LENGTH];
            int maxlen = SIDEBAR_WIDTH - maxlen_offset;
            memcpy(tmpnck, chat->peer_list[peer].name, maxlen);
            tmpnck[maxlen] = '\0';

            int namecolour = WHITE;

            if (chat->peer_list[i].status == TOX_USER_STATUS_AWAY)
                namecolour = YELLOW;
            else if (chat->peer_list[i].status == TOX_USER_STATUS_BUSY)
                namecolour = RED;

            /* Signify roles (e.g. founder, moderator) */
            const char *rolesig = "";
            int rolecolour = WHITE;

            if (chat->peer_list[i].role == TOX_GROUP_ROLE_FOUNDER) {
                rolesig = "&";
                rolecolour = BLUE;
            } else if (chat->peer_list[i].role == TOX_GROUP_ROLE_MODERATOR) {
                rolesig = "+";
                rolecolour = GREEN;
            } else if (chat->peer_list[i].role == TOX_GROUP_ROLE_OBSERVER) {
                rolesig = "-";
                rolecolour = MAGENTA;
            }

            wattron(ctx->sidebar, COLOR_PAIR(rolecolour) | A_BOLD);
            wprintw(ctx->sidebar, "%s", rolesig);
            wattroff(ctx->sidebar, COLOR_PAIR(rolecolour) | A_BOLD);

            wattron(ctx->sidebar, COLOR_PAIR(namecolour));
            wprintw(ctx->sidebar, "%s\n", tmpnck);
            wattroff(ctx->sidebar, COLOR_PAIR(namecolour));
        }

        pthread_mutex_unlock(&Winthread.lock);
    }

    int y, x;
    getyx(self->window, y, x);
    (void) x;
    int new_x = ctx->start ? x2 - 1 : wcswidth(ctx->line, ctx->pos);
    wmove(self->window, y + 1, new_x);

    wrefresh(self->window);

    if (self->help->active)
        help_onDraw(self);
}

static void groupchat_onInit(ToxWindow *self, Tox *m)
{
    int x2, y2;
    getmaxyx(self->window, y2, x2);

    ChatContext *ctx = self->chatwin;

    ctx->history = subwin(self->window, y2 - CHATBOX_HEIGHT + 1, x2 - SIDEBAR_WIDTH - 1, 0, 0);
    ctx->linewin = subwin(self->window, CHATBOX_HEIGHT, x2, y2 - CHATBOX_HEIGHT, 0);
    ctx->sidebar = subwin(self->window, y2 - CHATBOX_HEIGHT + 1, SIDEBAR_WIDTH, 0, x2 - SIDEBAR_WIDTH);

    ctx->hst = calloc(1, sizeof(struct history));
    ctx->log = calloc(1, sizeof(struct chatlog));

    if (ctx->log == NULL || ctx->hst == NULL)
        exit_toxic_err("failed in groupchat_onInit", FATALERR_MEMORY);

    line_info_init(ctx->hst);

    if (user_settings->autolog == AUTOLOG_ON) {
        char myid[TOX_ADDRESS_SIZE];
        tox_self_get_address(m, (uint8_t *) myid);
        log_enable(self->name, myid, NULL, ctx->log, LOG_GROUP);
    }

    execute(ctx->history, self, m, "/log", GLOBAL_COMMAND_MODE);

    scrollok(ctx->history, 0);
    wmove(self->window, y2 - CURS_Y_OFFSET, 0);
}

ToxWindow new_group_chat(Tox *m, uint32_t groupnum, const char *groupname, int length)
{
    ToxWindow ret;
    memset(&ret, 0, sizeof(ret));

    ret.active = true;
    ret.is_groupchat = true;

    ret.onKey = &groupchat_onKey;
    ret.onDraw = &groupchat_onDraw;
    ret.onInit = &groupchat_onInit;
    ret.onGroupMessage = &groupchat_onGroupMessage;
    ret.onGroupPeerlistUpdate = &groupchat_onGroupPeerlistUpdate;
    ret.onGroupPrivateMessage = &groupchat_onGroupPrivateMessage;
    ret.onGroupPeerJoin = &groupchat_onGroupPeerJoin;
    ret.onGroupPeerExit = &groupchat_onGroupPeerExit;
    ret.onGroupTopicChange = &groupchat_onGroupTopicChange;
    ret.onGroupPeerLimit = &groupchat_onGroupPeerLimit;
    ret.onGroupPrivacyState = &groupchat_onGroupPrivacyState;
    ret.onGroupPassword = &groupchat_onGroupPassword;
    ret.onGroupNickChange = &groupchat_onGroupNickChange;
    ret.onGroupStatusChange = &groupchat_onGroupStatusChange;
    ret.onGroupSelfJoin = &groupchat_onGroupSelfJoin;
    ret.onGroupRejected = &groupchat_onGroupRejected;
    ret.onGroupModeration = &groupchat_onGroupModeration;

    ChatContext *chatwin = calloc(1, sizeof(ChatContext));
    Help *help = calloc(1, sizeof(Help));

    if (chatwin == NULL || help == NULL)
        exit_toxic_err("failed in new_group_chat", FATALERR_MEMORY);

    ret.chatwin = chatwin;
    ret.help = help;

    ret.num = groupnum;
    ret.show_peerlist = true;
    ret.active_box = -1;

    if (groupname && length > 0)
        set_window_title(&ret, groupname, length);
    else
        snprintf(ret.name, sizeof(ret.name), "Group %d", groupnum);

    return ret;
}
