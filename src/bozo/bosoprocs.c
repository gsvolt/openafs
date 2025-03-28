/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#include <afsconfig.h>
#include <afs/param.h>
#include <afs/stds.h>

#include <roken.h>

#ifdef AFS_NT40_ENV
#include <sys/utime.h>
#endif /* AFS_NT40_ENV */

#include <rx/xdr.h>
#include <rx/rx.h>
#include <rx/rxkad.h>
#include <afs/cellconfig.h>
#include <afs/keys.h>
#include <afs/afsutil.h>
#include <afs/fileutil.h>
#include <afs/ktime.h>
#include <afs/audit.h>
#include <afs/kautils.h>

#include "bnode.h"
#include "bnode_internal.h"
#include "bosint.h"
#include "bosprototypes.h"

extern struct ktime bozo_nextRestartKT, bozo_nextDayKT;

extern struct afsconf_dir *bozo_confdir;
extern int bozo_newKTs;
extern int DoLogging;

afs_int32
SBOZO_GetRestartTime(struct rx_call *acall, afs_int32 atype, struct bozo_netKTime *aktime)
{
    afs_int32 code;

    BNODE_LOCK();

    code = 0;			/* assume success */
    switch (atype) {
    case 1:
	memcpy(aktime, &bozo_nextRestartKT, sizeof(struct ktime));
	break;

    case 2:
	memcpy(aktime, &bozo_nextDayKT, sizeof(struct ktime));
	break;

    default:
	code = BZDOM;
	break;
    }

    BNODE_UNLOCK();

    return code;
}

afs_int32
SBOZO_SetRestartTime(struct rx_call *acall, afs_int32 atype, struct bozo_netKTime *aktime)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    /* check for proper permissions */
    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing SetRestartTime\n", caller));

    code = 0;			/* assume success */
    switch (atype) {
    case 1:
	memcpy(&bozo_nextRestartKT, aktime, sizeof(struct ktime));
	break;

    case 2:
	memcpy(&bozo_nextDayKT, aktime, sizeof(struct ktime));
	break;

    default:
	code = BZDOM;
	break;
    }

    if (code == 0) {
	/* try to update the bozo init file */
	code = WriteBozoFile(0);
	bozo_newKTs = 1;
    }

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_SetRestartEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_Exec(struct rx_call *acall, char *acmd)
{

    char caller[MAXKTCNAMELEN];
    int code;

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (bozo_IsRestricted()) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing the shell command '%s'\n", caller, acmd));

    /*
     * The BOZO_Exec() interface does not include provision for reading
     * data and attaching it to stdin of the exec()'d process, or for
     * relaying stdout or stderr of the exec()'d process to the Rx reply
     * stream, so all we can do is relay the exit status as the RPC return
     * code.  In theory a new RPC could be defined that has such semantics
     * but given the ubiquity of ssh and fleet automation tooling it does
     * not seem necessary to incorporate such functionality into the OpenAFS
     * tooling directly.
     */
    code = system(acmd);
    osi_auditU(acall, BOS_ExecEvent, code, AUD_STR, acmd, AUD_END);

  fail:
    return code;
}

afs_int32
SBOZO_GetDates(struct rx_call *acall, char *aname, afs_int32 *atime,
	       afs_int32 *abakTime, afs_int32 *aoldTime)
{
    struct stat tstat;
    char *filepath = NULL, *fpBak = NULL, *fpOld = NULL;

    *atime = *abakTime = *aoldTime = 0;

    /* construct local path from canonical (wire-format) path */
    if (ConstructLocalBinPath(aname, &filepath)) {
	return 0;
    }
    if (asprintf(&fpBak, "%s.BAK", filepath) < 0) {
	fpBak = NULL;
	goto out;
    }
    if (asprintf(&fpOld, "%s.OLD", filepath) < 0) {
	fpOld = NULL;
	goto out;
    }

    if (!stat(filepath, &tstat)) {
	*atime = tstat.st_mtime;
    }

    if (!stat(fpBak, &tstat)) {
	*abakTime = tstat.st_mtime;
    }

    if (!stat(fpOld, &tstat)) {
	*aoldTime = tstat.st_mtime;
    }
out:
    free(fpOld);
    free(fpBak);
    free(filepath);
    return 0;
}

afs_int32
SBOZO_UnInstall(struct rx_call *acall, char *aname)
{
    char *filepath = NULL;
    char *fpOld = NULL, *fpBak = NULL;
    afs_int32 code;
    char caller[MAXKTCNAMELEN];
    struct stat tstat;

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	osi_auditU(acall, BOS_UnInstallEvent, code, AUD_STR, aname, AUD_END);
	return code;
    }
    if (bozo_IsRestricted()) {
	code = BZACCESS;
	osi_auditU(acall, BOS_UnInstallEvent, code, AUD_STR, aname, AUD_END);
	return code;
    }

    /* construct local path from canonical (wire-format) path */
    if (ConstructLocalBinPath(aname, &filepath)) {
	return BZNOENT;
    }

    if (DoLogging)
	ViceLog(0, ("%s is executing UnInstall '%s'\n", caller, filepath));

    if (asprintf(&fpBak, "%s.BAK", filepath) < 0) {
	code = BZIO;
	fpBak = NULL;
	goto out;
    }
    if (asprintf(&fpOld, "%s.OLD", filepath) < 0) {
	code = BZIO;
	fpOld = NULL;
	goto out;
    }

    code = rk_rename(fpBak, filepath);
    if (code) {
	/* can't find .BAK, try .OLD */
	code = rk_rename(fpOld, filepath);
	if (code && errno == ENOENT)	/* If doesn't exist don't fail */
	    code = 0;
    } else {
	/* now rename .OLD to .BAK */
	if (stat(fpOld, &tstat) == 0)
	    code = rk_rename(fpOld, fpBak);
    }
    if (code)
	code = errno;

out:
    osi_auditU(acall, BOS_UnInstallEvent, code, AUD_STR, filepath, AUD_END);
    free(fpBak);
    free(fpOld);
    free(filepath);

    return code;
}

static void
SaveOldFiles(char *aname)
{
    afs_int32 code;
    char *bbuffer = NULL, *obuffer = NULL;
    struct stat tstat;
    afs_int32 now;
    int oldExists;
    int bakExists;
    afs_int32 bakTime;
    const int SECONDS_PER_WEEK = 7 * 24 * 3600; /* 7 days old */

    now = FT_ApproxTime();

    code = stat(aname, &tstat);
    if (code < 0)
	return;			/* can't stat file */

    if (asprintf(&bbuffer, "%s.BAK", aname) < 0)
	return;

    if (asprintf(&obuffer, "%s.OLD", aname) < 0) {
	obuffer = NULL;
	goto out;
    }

    code = stat(obuffer, &tstat);	/* Check if .OLD file exists. */
    if (code != 0) {
	oldExists = 0;
    } else {
	oldExists = 1;
    }

    code = stat(bbuffer, &tstat);	/* Check if .BAK file exists. */
    if (code != 0) {
	bakExists = 0;
    } else {
	bakExists = 1;
	bakTime = tstat.st_mtime;
    }

    if (bakExists == 1 && (oldExists == 0 || bakTime < now - SECONDS_PER_WEEK)) {
	/* No .OLD file or .BAK is at least a week old. */
	rk_rename(bbuffer, obuffer);
    }

    /* finally rename to .BAK extension */
    rk_rename(aname, bbuffer);

out:
    free(bbuffer);
    free(obuffer);
}

afs_int32
SBOZO_Install(struct rx_call *acall, char *aname, afs_int32 asize, afs_int32 mode, afs_int32 amtime)
{
    afs_int32 code, ret = 0;
    int fd = -1;
    afs_int32 len;
    afs_int32 total;
#ifdef AFS_NT40_ENV
    struct _utimbuf utbuf;
#else
    struct timeval tvb[2];
#endif
    char *filepath = NULL, *fpNew = NULL, *tbuffer = NULL;
    char caller[MAXKTCNAMELEN];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller))
	return BZACCESS;
    if (bozo_IsRestricted())
	return BZACCESS;

    /* construct local path from canonical (wire-format) path */
    if (ConstructLocalBinPath(aname, &filepath)) {
	return BZNOENT;
    }
    if (asprintf(&fpNew, "%s.NEW", filepath) < 0) {
	ret = BZIO;
	fpNew = NULL;
	goto out;
    }
    tbuffer = malloc(AFSDIR_PATH_MAX);
    if (tbuffer == NULL) {
	ret = BZIO;
	goto out;
    }

    if (DoLogging)
	ViceLog(0, ("%s is executing Install '%s'\n", caller, filepath));

    /* open file */
    fd = open(fpNew, O_CREAT | O_RDWR | O_TRUNC, 0777);
    if (fd < 0) {
	ret =  errno;
	goto out;
    }
    total = 0;
    while (1) {
	len = rx_Read(acall, tbuffer, sizeof(tbuffer));
	if (len < 0) {
	    unlink(fpNew);
	    ret =  102;
	    goto out;
	}
	if (len == 0)
	    break;		/* no more input */
	code = write(fd, tbuffer, len);
	if (code != len) {
	    unlink(fpNew);
	    ret =  100;
	    goto out;
	}
	total += len;		/* track total written for safety check at end */
    }
    if (asize != total) {
	unlink(fpNew);
	ret =  101;		/* wrong size */
	goto out;
    }

    /* save old files */
    SaveOldFiles(filepath);	/* don't care if it works, still install */

    /* all done, rename to final name */
    code = (rk_rename(fpNew, filepath) ? errno : 0);

    /* label file with same time for our sanity */
#ifdef AFS_NT40_ENV
    utbuf.actime = utbuf.modtime = amtime;
    _utime(filepath, &utbuf);
#else
    tvb[0].tv_sec = tvb[1].tv_sec = amtime;
    tvb[0].tv_usec = tvb[1].tv_usec = 0;
    utimes(filepath, tvb);
#endif /* AFS_NT40_ENV */

    if (mode)
	chmod(filepath, mode);

    if (code < 0) {
	osi_auditU(acall, BOS_InstallEvent, code, AUD_STR, filepath, AUD_END);
	ret = errno;
	goto out;
    }
    ret = 0;
out:
    if (fd >= 0)
	close(fd);
    free(filepath);
    free(fpNew);
    free(tbuffer);
    return ret;
}

afs_int32
SBOZO_SetCellName(struct rx_call *acall, char *aname)
{
    struct afsconf_cell tcell;
    afs_int32 code;
    char caller[MAXKTCNAMELEN];
    char clones[MAXHOSTSPERCELL];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing SetCellName '%s'\n", caller, aname));

    code =
	afsconf_GetExtendedCellInfo(bozo_confdir, NULL, NULL, &tcell,
				    clones);
    if (code)
	goto fail;

    /* Check that tcell has enough space for the new cellname. */
    if (strlen(aname) > sizeof tcell.name - 1) {
	ViceLog(0,
	    ("ERROR: SetCellName: cell name '%s' exceeds %ld bytes (cell name not changed)\n",
	     aname, (long)(sizeof tcell.name - 1)));
	code = BZDOM;
	goto fail;
    }

    strcpy(tcell.name, aname);
    code =
	afsconf_SetExtendedCellInfo(bozo_confdir, AFSDIR_SERVER_ETC_DIRPATH,
				    &tcell, clones);

  fail:
    osi_auditU(acall, BOS_SetCellEvent, code, AUD_STR, aname, AUD_END);
    return code;
}

afs_int32
SBOZO_GetCellName(struct rx_call *acall, char **aname)
{
    afs_int32 code;
    char tname[MAXCELLCHARS];

    code = afsconf_GetLocalCell(bozo_confdir, tname, sizeof(tname));
    if (code == 0) {
	*aname = strdup(tname);
	if (*aname == NULL)
	    code = BZIO;
    }

    return code;
}

afs_int32
SBOZO_GetCellHost(struct rx_call *acall, afs_uint32 awhich, char **aname)
{
    afs_int32 code;
    struct afsconf_cell tcell;
    char *tp;
    char clones[MAXHOSTSPERCELL];

    code =
	afsconf_GetExtendedCellInfo(bozo_confdir, NULL, NULL, &tcell,
				    clones);
    if (code)
	goto fail;

    if (awhich >= tcell.numServers) {
	code = BZDOM;
	goto fail;
    }

    tp = tcell.hostName[awhich];
    if (clones[awhich]) {
	if (asprintf(aname, "[%s]", tp) < 0)
	    *aname = NULL;
    } else
	*aname = strdup(tp);
    if (*aname == NULL) {
	code = BZIO;
	goto fail;
    }
    goto done;

  fail:
    *aname = malloc(1);	/* return fake string */
    **aname = 0;

  done:
    return code;
}

afs_int32
SBOZO_DeleteCellHost(struct rx_call *acall, char *aname)
{
    afs_int32 code;
    struct afsconf_cell tcell;
    afs_int32 which;
    int i;
    char caller[MAXKTCNAMELEN];
    char clones[MAXHOSTSPERCELL];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing DeleteCellHost '%s'\n", caller, aname));

    code =
	afsconf_GetExtendedCellInfo(bozo_confdir, NULL, NULL, &tcell,
				    clones);
    if (code)
	goto fail;

    which = -1;
    for (i = 0; i < tcell.numServers; i++) {
	if (strcmp(tcell.hostName[i], aname) == 0) {
	    which = i;
	    break;
	}
    }

    if (which < 0) {
	code = BZNOENT;
	goto fail;
    }

    memset(&tcell.hostAddr[which], 0, sizeof(struct sockaddr_in));
    memset(tcell.hostName[which], 0, MAXHOSTCHARS);
    code =
	afsconf_SetExtendedCellInfo(bozo_confdir, AFSDIR_SERVER_ETC_DIRPATH,
				    &tcell, clones);

  fail:
    osi_auditU(acall, BOS_DeleteHostEvent, code, AUD_STR, aname, AUD_END);
    return code;
}

afs_int32
SBOZO_AddCellHost(struct rx_call *acall, char *aname)
{
    afs_int32 code;
    struct afsconf_cell tcell;
    afs_int32 which;
    int i;
    char caller[MAXKTCNAMELEN];
    char clones[MAXHOSTSPERCELL];
    char *n;
    char isClone = 0;

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing AddCellHost '%s'\n", caller, aname));

    code =
	afsconf_GetExtendedCellInfo(bozo_confdir, NULL, NULL, &tcell,
				    clones);
    if (code)
	goto fail;

    n = aname;
    if (*n == '[') {
	*(n + strlen(n) - 1) = 0;
	++n;
	isClone = 1;
    }

    which = -1;
    for (i = 0; i < tcell.numServers; i++) {
	if (strcmp(tcell.hostName[i], n) == 0) {
	    which = i;
	    break;
	}
    }
    if (which < 0) {
	which = tcell.numServers;
	tcell.numServers++;

	/*
	 * Check that tcell has enough space for an additional host.
	 *
	 * We assume that tcell.hostAddr[] and tcell.hostName[] have the
	 * same number of entries.
	 */
	if (tcell.numServers >
	    sizeof tcell.hostAddr / sizeof tcell.hostAddr[0]) {
	    ViceLog(0,
		("ERROR: AddCellHost: attempt to add more than %ld database servers (database server '%s' not added)\n",
		 (long)(sizeof tcell.hostAddr / sizeof tcell.hostAddr[0]),
		 aname));
	    code = BZDOM;
	    goto fail;
	}

	/* Check that tcell has enough space for the new hostname. */
	if (strlen(aname) > sizeof tcell.hostName[0] - 1) {
	    ViceLog(0,
		("ERROR: AddCellHost: host name '%s' exceeds %ld bytes (not added)\n",
		 aname, (long)(sizeof tcell.hostName[0] - 1)));
	    code = BZDOM;
	    goto fail;
	}
    }

    memset(&tcell.hostAddr[which], 0, sizeof(struct sockaddr_in));
    strcpy(tcell.hostName[which], n);
    clones[which] = isClone;
    code =
	afsconf_SetExtendedCellInfo(bozo_confdir, AFSDIR_SERVER_ETC_DIRPATH,
				    &tcell, clones);

  fail:
    osi_auditU(acall, BOS_AddHostEvent, code, AUD_STR, aname, AUD_END);
    return code;
}

afs_int32
SBOZO_ListKeys(struct rx_call *acall, afs_int32 an, afs_int32 *akvno,
	       struct bozo_key *akey, struct bozo_keyInfo *akeyinfo)
{
    struct afsconf_keys tkeys;
    afs_int32 code;
    struct stat tstat;
    int noauth = 0;
    char caller[MAXKTCNAMELEN];
    rxkad_level enc_level = rxkad_clear;

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing ListKeys\n", caller));

    code = afsconf_GetKeys(bozo_confdir, &tkeys);
    if (code)
	goto fail;

    if (tkeys.nkeys <= an) {
	code = BZDOM;
	goto fail;
    }
    *akvno = tkeys.key[an].kvno;
    memset(akeyinfo, 0, sizeof(struct bozo_keyInfo));

    noauth = afsconf_GetNoAuthFlag(bozo_confdir);
    rxkad_GetServerInfo(rx_ConnectionOf(acall), &enc_level, 0, 0, 0, 0, 0);
    /*
     * only return actual keys in noauth or if this is an encrypted connection
     */

    if ((noauth) || (enc_level == rxkad_crypt)) {
	memcpy(akey, tkeys.key[an].key, 8);
    } else
	memset(akey, 0, 8);

    code = stat(AFSDIR_SERVER_KEY_FILEPATH, &tstat);
    if (code == 0) {
	akeyinfo->mod_sec = tstat.st_mtime;
    }

    /* This will return an error if the key is 'bad' (bad checksum, weak DES
     * key, etc). But we don't care, since we can still return the other
     * information about the key, so ignore the result. */
    (void)ka_KeyCheckSum(tkeys.key[an].key, &akeyinfo->keyCheckSum);

  fail:
    if (noauth)
	osi_auditU(acall, BOS_UnAuthListKeysEvent, code, AUD_END);
    osi_auditU(acall, BOS_ListKeysEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_AddKey(struct rx_call *acall, afs_int32 an, struct bozo_key *akey)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];
    rxkad_level enc_level = rxkad_clear;
    int noauth;

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    noauth = afsconf_GetNoAuthFlag(bozo_confdir);
    rxkad_GetServerInfo(rx_ConnectionOf(acall), &enc_level, 0, 0, 0, 0, 0);
    if ((!noauth) && (enc_level != rxkad_crypt)) {
	code = BZENCREQ;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing AddKey\n", caller));

    code = afsconf_AddKey(bozo_confdir, an, akey->data, 0);
    if (code == AFSCONF_KEYINUSE)
	code = BZKEYINUSE;	/* Unique code for afs rpc calls */
  fail:
    osi_auditU(acall, BOS_AddKeyEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_SetNoAuthFlag(struct rx_call *acall, afs_int32 aflag)
{
    afs_int32 code = 0;
    char caller[MAXKTCNAMELEN];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing Set No Authentication\n", caller));

    afsconf_SetNoAuthFlag(bozo_confdir, aflag);

  fail:
    osi_auditU(acall, BOS_SetNoAuthEvent, code, AUD_LONG, aflag, AUD_END);
    return code;
}

afs_int32
SBOZO_DeleteKey(struct rx_call *acall, afs_int32 an)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing DeleteKey\n", caller));

    code = afsconf_DeleteKey(bozo_confdir, an);

  fail:
    osi_auditU(acall, BOS_DeleteKeyEvent, code, AUD_END);
    return code;
}


afs_int32
SBOZO_ListSUsers(struct rx_call *acall, afs_int32 an, char **aname)
{
    afs_int32 code;
    char *tp;

    tp = *aname = malloc(256);
    *tp = 0;			/* in case getnthuser doesn't null-terminate the string */
    code = afsconf_GetNthUser(bozo_confdir, an, tp, 256);

  /* fail: */
    osi_auditU(acall, BOS_ListSUserEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_AddSUser(struct rx_call *acall, char *aname)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing Add SuperUser '%s'\n", caller, aname));

    code = afsconf_AddUser(bozo_confdir, aname);

  fail:
    osi_auditU(acall, BOS_AddSUserEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_DeleteSUser(struct rx_call *acall, char *aname)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }

    if (DoLogging)
	ViceLog(0, ("%s is executing Delete SuperUser '%s'\n", caller, aname));

    code = afsconf_DeleteUser(bozo_confdir, aname);

  fail:
    osi_auditU(acall, BOS_DeleteSUserEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_CreateBnode(struct rx_call *acall, char *atype, char *ainstance,
		  char *ap1, char *ap2, char *ap3, char *ap4, char *ap5,
                  char *notifier)
{
    struct bnode *tb;
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (bozo_IsRestricted()) {
	const char *salvpath = AFSDIR_CANONICAL_SERVER_SALVAGER_FILEPATH;
	/* for DAFS, 'bos salvage' will pass "salvageserver -client" instead */
	const char *salsrvpath = AFSDIR_CANONICAL_SERVER_SALSRV_FILEPATH " -client ";

	/* still allow 'bos salvage' to work */
	if (strcmp(atype, "cron") || strcmp(ainstance, "salvage-tmp")
	    || strcmp(ap2, "now")
	    || (strncmp(ap1, salvpath, strlen(salvpath))
	        && strncmp(ap1, salsrvpath, strlen(salsrvpath)))) {

	    code = BZACCESS;
	    goto fail;
	}
    }

    code =
	bnode_Create(atype, ainstance, &tb, ap1, ap2, ap3, ap4, ap5, notifier,
		     BSTAT_NORMAL, 1);
    if (!code)
	bnode_SetStat(tb, BSTAT_NORMAL);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_CreateBnodeEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_WaitAll(struct rx_call *acall)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }

    if (DoLogging)
	ViceLog(0, ("%s is executing Wait for All\n", caller));

    code = bnode_WaitAll();

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_WaitAllEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_DeleteBnode(struct rx_call *acall, char *ainstance)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (bozo_IsRestricted()) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing DeleteBnode '%s'\n", caller, ainstance));

    code = bnode_DeleteName(ainstance);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_DeleteBnodeEvent, code, AUD_STR, ainstance,
	       AUD_END);
    return code;
}

static int
swproc(struct bnode *abnode, void *arock)
{
    if (abnode->goal == BSTAT_NORMAL)
	return 0;		/* this one's not shutting down */
    /* otherwise, we are shutting down */
    bnode_Hold(abnode);
    bnode_WaitStatus(abnode, BSTAT_SHUTDOWN);
    bnode_Release(abnode);
    return 0;			/* don't stop apply function early, no matter what */
}

static int
stproc(struct bnode *abnode, void *arock)
{
    if (abnode->fileGoal == BSTAT_SHUTDOWN)
	return 0;		/* don't do these guys */

    bnode_Hold(abnode);
    bnode_ResetErrorCount(abnode);
    bnode_SetStat(abnode, BSTAT_NORMAL);
    bnode_Release(abnode);
    return 0;
}

static int
sdproc(struct bnode *abnode, void *arock)
{
    bnode_Hold(abnode);
    bnode_SetStat(abnode, BSTAT_SHUTDOWN);
    bnode_Release(abnode);
    return 0;
}

/* shutdown and leave down */
afs_int32
SBOZO_ShutdownAll(struct rx_call *acall)
{
    /* iterate over all bnodes, setting the status to temporarily disabled */
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    /* check for authorization */
    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing ShutdownAll\n", caller));

    code = bnode_ApplyInstance(sdproc, NULL);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_ShutdownAllEvent, code, AUD_END);
    return code;
}

/* shutdown and restart */
afs_int32
SBOZO_RestartAll(struct rx_call *acall)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing RestartAll\n", caller));

    /* start shutdown of all processes */
    code = bnode_ApplyInstance(sdproc, NULL);
    if (code)
	goto fail;

    /* wait for all done */
    code = bnode_ApplyInstance(swproc, NULL);
    if (code)
	goto fail;

    /* start them up again */
    code = bnode_ApplyInstance(stproc, NULL);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_RestartAllEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_ReBozo(struct rx_call *acall)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    /* acall is null if called internally to restart bosserver */
    if (acall && !afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing ReBozo\n", caller));

    /* start shutdown of all processes */
    code = bnode_ApplyInstance(sdproc, NULL);
    if (code)
	goto fail;

    /* wait for all done */
    code = bnode_ApplyInstance(swproc, NULL);
    if (code)
	goto fail;

    BNODE_UNLOCK();

    if (acall)
	osi_auditU(acall, BOS_RebozoEvent, code, AUD_END);
    else
	osi_audit(BOS_RebozoIntEvent, code, AUD_END);

    if (acall)
	rx_EndCall(acall, 0);	/* try to get it done */
    rx_Finalize();
    bozo_ReBozo();		/* this reexecs us, and doesn't return, of course */

  fail:
    BNODE_UNLOCK();
    /* Differentiate between external and internal ReBozo; prevents AFS_Aud_NoCall event */
    if (acall)
	osi_auditU(acall, BOS_RebozoEvent, code, AUD_END);
    else
	osi_audit(BOS_RebozoIntEvent, code, AUD_END);
    return code;		/* should only get here in unusual circumstances */
}

/* make sure all are running */
afs_int32
SBOZO_StartupAll(struct rx_call *acall)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing StartupAll\n", caller));
    code = bnode_ApplyInstance(stproc, NULL);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_StartupAllEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_Restart(struct rx_call *acall, char *ainstance)
{
    struct bnode *tb;
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing Restart '%s'\n", caller, ainstance));

    tb = bnode_FindInstance(ainstance);
    if (!tb) {
	code = BZNOENT;
	goto fail;
    }

    bnode_Hold(tb);
    bnode_SetStat(tb, BSTAT_SHUTDOWN);
    code = bnode_WaitStatus(tb, BSTAT_SHUTDOWN);	/* this can fail */
    bnode_ResetErrorCount(tb);
    bnode_SetStat(tb, BSTAT_NORMAL);
    bnode_Release(tb);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_RestartEvent, code, AUD_STR, ainstance, AUD_END);
    return code;
}

/* set temp status */
afs_int32
SBOZO_SetTStatus(struct rx_call *acall, char *ainstance, afs_int32 astatus)
{
    struct bnode *tb;
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing SetTempStatus '%s'\n", caller, ainstance));

    tb = bnode_FindInstance(ainstance);
    if (!tb) {
	code = BZNOENT;
	goto fail;
    }
    bnode_Hold(tb);
    bnode_ResetErrorCount(tb);
    code = bnode_SetStat(tb, astatus);
    bnode_Release(tb);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_SetTempStatusEvent, code, AUD_STR, ainstance,
	       AUD_END);
    return code;
}

afs_int32
SBOZO_SetStatus(struct rx_call *acall, char *ainstance, afs_int32 astatus)
{
    struct bnode *tb;
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing SetStatus '%s' (status = %d)\n", caller,
		    ainstance, astatus));

    tb = bnode_FindInstance(ainstance);
    if (!tb) {
	code = BZNOENT;
	goto fail;
    }
    bnode_Hold(tb);
    bnode_SetFileGoal(tb, astatus);
    code = bnode_SetStat(tb, astatus);
    bnode_Release(tb);

  fail:
    BNODE_UNLOCK();
    osi_auditU(acall, BOS_SetStatusEvent, code, AUD_STR, ainstance, AUD_END);
    return code;
}

afs_int32
SBOZO_GetStatus(struct rx_call *acall, char *ainstance, afs_int32 *astat,
		char **astatDescr)
{
    struct bnode *tb;
    afs_int32 code;

    BNODE_LOCK();

    tb = bnode_FindInstance(ainstance);
    if (!tb) {
	BNODE_UNLOCK();
	return BZNOENT;
    }

    bnode_Hold(tb);
    code = bnode_GetStat(tb, astat);
    if (code == 0)
	code = bnode_GetString(tb, astatDescr);
    bnode_Release(tb);

    BNODE_UNLOCK();

    return code;
}

struct eidata {
    char *iname;
    int counter;
};

static int
eifunc(struct bnode *abnode, void *param)
{
    struct eidata *arock = (struct eidata *)param;

    if (arock->counter-- == 0) {
	/* done */
	arock->iname = strdup(abnode->name);
	return 1;
    } else {
	/* not there yet */
	return 0;
    }
}

static int
ZapFile(const char *adir, const char *aname)
{
    char tbuffer[256];
    if (snprintf(tbuffer, 256, "%s/%s", adir, aname)<256)
	return unlink(tbuffer);
    else
	return -1;
}

afs_int32
SBOZO_Prune(struct rx_call *acall, afs_int32 aflags)
{
    afs_int32 code;
    DIR *dirp;
    struct dirent *tde;
    int i;
    char caller[MAXKTCNAMELEN];

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (bozo_IsRestricted()) {
	code = BZACCESS;
	goto fail;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing Prune (flags=%d)\n", caller, aflags));

    /* first scan AFS binary directory */
    dirp = opendir(AFSDIR_SERVER_BIN_DIRPATH);
    if (dirp) {
	for (tde = readdir(dirp); tde; tde = readdir(dirp)) {
	    i = strlen(tde->d_name);
	    if (aflags & BOZO_PRUNEOLD) {
		if (i >= 4 && strncmp(tde->d_name + i - 4, ".OLD", 4) == 0)
		    ZapFile(AFSDIR_SERVER_BIN_DIRPATH, tde->d_name);
	    }
	    if (aflags & BOZO_PRUNEBAK) {
		if (i >= 4 && strncmp(tde->d_name + i - 4, ".BAK", 4) == 0)
		    ZapFile(AFSDIR_SERVER_BIN_DIRPATH, tde->d_name);
	    }
	}
	closedir(dirp);
    }

    /* then scan AFS log directory */
    dirp = opendir(AFSDIR_SERVER_LOGS_DIRPATH);
    if (dirp) {
	for (tde = readdir(dirp); tde; tde = readdir(dirp)) {
	    if (aflags & BOZO_PRUNECORE) {
		if (strncmp(tde->d_name, AFSDIR_CORE_FILE, 4) == 0)
		    ZapFile(AFSDIR_SERVER_LOGS_DIRPATH, tde->d_name);
	    }
	}
	closedir(dirp);
    }
    code = 0;

  fail:
    osi_auditU(acall, BOS_PruneLogs, code, AUD_END);
    return code;
}

afs_int32
SBOZO_EnumerateInstance(struct rx_call *acall, afs_int32 anum,
		        char **ainstance)
{
    struct eidata tdata;

    if (anum < 0)
	return BZDOM;

    tdata.counter = anum;
    tdata.iname = NULL;
    BNODE_LOCK();
    bnode_ApplyInstance(eifunc, &tdata);
    BNODE_UNLOCK();
    if (tdata.counter >= 0)
	return BZDOM;		/* anum > # of actual instances */
    if (tdata.iname == NULL)
	return BZIO;

    *ainstance = tdata.iname;
    return 0;
}

struct bozo_bosEntryStats bozo_bosEntryStats[] = {
    {NULL, 1, 1, 0755, 02},	/* AFSDIR_SERVER_AFS_DIRPATH    */
    {NULL, 1, 1, 0755, 02},	/* AFSDIR_SERVER_ETC_DIRPATH    */
    {NULL, 1, 1, 0755, 02},	/* AFSDIR_SERVER_BIN_DIRPATH    */
    {NULL, 1, 1, 0755, 02},	/* AFSDIR_SERVER_LOGS_DIRPATH   */
    {NULL, 1, 0, 0700, 07},	/* AFSDIR_SERVER_BACKUP_DIRPATH */
    {NULL, 1, 1, 0700, 07},	/* AFSDIR_SERVER_DB_DIRPATH     */
    {NULL, 1, 1, 0700, 07},	/* AFSDIR_SERVER_LOCAL_DIRPATH  */
    {NULL, 0, 1, 0600, 07},	/* AFSDIR_SERVER_KEY_FILEPATH   */
    {NULL, 0, 1, 0600, 03},	/* AFSDIR_SERVER_ULIST_FILEPATH */
    {NULL, 0, 1, 0600, 07},	/* AFSDIR_SERVER_RXKAD_KEYTAB_FILEPATH   */
    {NULL, 0, 1, 0600, 07}	/* AFSDIR_SERVER_EXT_KEY_FILEPATH   */
};
int bozo_nbosEntryStats =
    sizeof(bozo_bosEntryStats) / sizeof(bozo_bosEntryStats[0]);

/* This function performs initialization of the bozo_bosEntrystats[]
 * array. This array contains the list of dirs that the bosserver
 * is interested in along with their recommended permissions
 * NOTE: This initialization is a bit ugly. This was caused because
 * the path names require procedural as opposed to static initialization.
 * The other fields in the struct are however, statically initialized.
 */
int
initBosEntryStats(void)
{
    bozo_bosEntryStats[0].path = AFSDIR_SERVER_AFS_DIRPATH;
    bozo_bosEntryStats[1].path = AFSDIR_SERVER_ETC_DIRPATH;
    bozo_bosEntryStats[2].path = AFSDIR_SERVER_BIN_DIRPATH;
    bozo_bosEntryStats[3].path = AFSDIR_SERVER_LOGS_DIRPATH;
    bozo_bosEntryStats[4].path = AFSDIR_SERVER_BACKUP_DIRPATH;
    bozo_bosEntryStats[5].path = AFSDIR_SERVER_DB_DIRPATH;
    bozo_bosEntryStats[6].path = AFSDIR_SERVER_LOCAL_DIRPATH;
    bozo_bosEntryStats[7].path = AFSDIR_SERVER_KEY_FILEPATH;
    bozo_bosEntryStats[8].path = AFSDIR_SERVER_ULIST_FILEPATH;
    bozo_bosEntryStats[9].path = AFSDIR_SERVER_RXKAD_KEYTAB_FILEPATH;
    bozo_bosEntryStats[10].path = AFSDIR_SERVER_EXT_KEY_FILEPATH;

    return 0;
}

/* StatEachEntry - If it's not there, it is okay.  If anything else goes wrong
 * complain.  Otherwise check permissions: shouldn't allow write or (usually)
 * read. */

static int
StatEachEntry(IN struct bozo_bosEntryStats *stats)
{
    struct stat info;
    if (stat(stats->path, &info)) {
	if (errno == ENOENT)
	    return 1;		/* no such entry: just ignore it */
	return 0;		/* something else went wrong */
    } else {
	int rights;
	if (((info.st_mode & S_IFDIR) != 0) != stats->dir)
	    return 0;		/* not expected type */
	if (stats->rootOwner && (info.st_uid != 0))
	    return 0;		/* not owned by root */
	rights = (info.st_mode & 0000777);
	if ((rights & stats->reqPerm) != stats->reqPerm)
	    return 0;		/* required permissions not present */
	if ((rights & stats->proPerm) != 0)
	    return 0;		/* prohibited permissions present */
    }
    return 1;
}

/*
 * DirAccessOK - checks the mode bits on the AFS dir and decendents and
 * returns 0 if some are not OK and 1 otherwise.  For efficiency, it doesn't do
 * this check more often than every 5 seconds.
 *
 * @pre BNODE_LOCK must be held
 */
int
DirAccessOK(void)
{
#ifdef AFS_NT40_ENV
    /* underlying filesystem may not support directory protection */
    return 1;
#else
    static afs_uint32 lastTime = 0;
    static int lastResult = -1;
    afs_uint32 now = FT_ApproxTime();
    int result;
    int i;

    BNODE_ASSERT_LOCK();

    if ((now - lastTime) < 5)
	return lastResult;
    lastTime = now;

    result = 1;
    for (i = 0; i < bozo_nbosEntryStats; i++) {
	struct bozo_bosEntryStats *e = &bozo_bosEntryStats[i];
	if (!StatEachEntry(e)) {
	    ViceLog(0,
		    ("unhappy with %s which is a %s that should "
		     "have at least rights %o, at most rights %o %s\n",
		     e->path, e->dir ? "dir" : "file", e->reqPerm,
		     (~e->proPerm & 0777),
		     e->rootOwner ? ", owned by root" : ""));
	    result = 0;
	}
    }

    if (result != lastResult) {	/* log changes */
	ViceLog(0, ("Server directory access is %sokay\n",
		    (result ? "" : "not ")));
    }
    lastResult = result;
    return lastResult;
#endif /* AFS_NT40_ENV */
}

int
GetRequiredDirPerm(const char *path)
{
    int i;
    for (i = 0; i < bozo_nbosEntryStats; i++)
	if (strcmp(path, bozo_bosEntryStats[i].path) == 0)
	    return bozo_bosEntryStats[i].reqPerm;
    return -1;
}

afs_int32
SBOZO_GetInstanceInfo(IN struct rx_call *acall,
		      IN char *ainstance,
		      OUT char **atype,
		      OUT struct bozo_status *astatus)
{
    struct bnode *tb;

    BNODE_LOCK();

    tb = bnode_FindInstance(ainstance);
    if (!tb) {
	BNODE_UNLOCK();
	return BZNOENT;
    }
    if (tb->type)
	*atype = strdup(tb->type->name);
    else
	*atype = strdup("");
    if (*atype == NULL) {
	BNODE_UNLOCK();
	return BZIO;
    }

    memset(astatus, 0, sizeof(struct bozo_status));	/* good defaults */
    astatus->goal = tb->goal;
    astatus->fileGoal = tb->fileGoal;
    astatus->procStartTime = tb->procStartTime;
    astatus->procStarts = tb->procStarts;
    astatus->lastAnyExit = tb->lastAnyExit;
    astatus->lastErrorExit = tb->lastErrorExit;
    astatus->errorCode = tb->errorCode;
    astatus->errorSignal = tb->errorSignal;
    if (tb->flags & BNODE_ERRORSTOP)
	astatus->flags |= BOZO_ERRORSTOP;
    if (bnode_HasCore(tb))
	astatus->flags |= BOZO_HASCORE;
    if (!DirAccessOK())
	astatus->flags |= BOZO_BADDIRACCESS;
    BNODE_UNLOCK();
    return 0;
}

afs_int32
SBOZO_GetInstanceParm(struct rx_call *acall,
		      char *ainstance,
		      afs_int32 anum,
		      char **aparm)
{
    struct bnode *tb;
    afs_int32 code;

    BNODE_LOCK();
    tb = bnode_FindInstance(ainstance);
    if (!tb) {
	BNODE_UNLOCK();
	return BZNOENT;
    }
    bnode_Hold(tb);
    if (anum == 999) {
	code = bnode_GetNotifier(tb, aparm);
    } else {
	code = bnode_GetParm(tb, anum, aparm);
    }
    bnode_Release(tb);

    BNODE_UNLOCK();

    /* Not Currently Audited */
    return code;
}

afs_int32
SBOZO_GetLog(struct rx_call *acall, char *aname)
{
    afs_int32 code;
    FILE *tfile;
    int tc;
    char *logpath;
    char buffer;
    char caller[MAXKTCNAMELEN];

    /* Check access since 'aname' could specify a file outside of the
     * AFS log directory (which is bosserver's working directory).
     */
    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto fail;
    }
    if (bozo_IsRestricted() && strchr(aname, '/') != NULL
	&& strcmp(aname, AFSDIR_CANONICAL_SERVER_SLVGLOG_FILEPATH)) {
	code = BZACCESS;
	goto fail;
    }

    /* construct local path from canonical (wire-format) path */
    if (ConstructLocalLogPath(aname, &logpath)) {
	return BZNOENT;
    }
    if (DoLogging)
	ViceLog(0, ("%s is executing GetLog '%s'\n", caller, logpath));
    tfile = fopen(logpath, "r");
    free(logpath);

    if (!tfile) {
	return BZNOENT;
    }

    while (1) {
	tc = getc(tfile);
	if (tc == 0)
	    continue;		/* our termination condition on other end */
	if (tc == EOF)
	    break;
	buffer = tc;
	if (rx_Write(acall, &buffer, 1) != 1) {
	    fclose(tfile);
	    code = BZNET;
	    goto fail;
	}
    }

    /* all done, cleanup and return */
    fclose(tfile);

    /* write out the end delimeter */
    buffer = 0;
    if (rx_Write(acall, &buffer, 1) != 1)
	return BZNET;
    code = 0;

  fail:
    osi_auditU(acall, BOS_GetLogsEvent, code, AUD_END);
    return code;
}

afs_int32
SBOZO_GetInstanceStrings(struct rx_call *acall, char *abnodeName,
			 char **as1, char **as2, char **as3, char **as4)
{
    struct bnode *tb;

    BNODE_LOCK();

    *as2 = malloc(1);
    **as2 = 0;
    *as3 = malloc(1);
    **as3 = 0;
    *as4 = malloc(1);
    **as4 = 0;
    tb = bnode_FindInstance(abnodeName);
    if (!tb)
	goto fail;

    /* now, return the appropriate error string, if any */
    if (tb->lastErrorName) {
	*as1 = strdup(tb->lastErrorName);
    } else {
	*as1 = malloc(1);
	**as1 = 0;
    }
    BNODE_UNLOCK();
    return 0;

  fail:
    BNODE_UNLOCK();
    *as1 = malloc(1);
    **as1 = 0;
    return BZNOENT;
}

afs_int32
SBOZO_GetRestrictedMode(struct rx_call *acall, afs_int32 *arestmode)
{
    *arestmode = bozo_IsRestricted();
    return 0;
}

afs_int32
SBOZO_SetRestrictedMode(struct rx_call *acall, afs_int32 arestmode)
{
    afs_int32 code;
    char caller[MAXKTCNAMELEN];

    BNODE_LOCK();

    if (!afsconf_SuperUser(bozo_confdir, acall, caller)) {
	code = BZACCESS;
	goto done;
    }
    if (bozo_IsRestricted()) {
	code = BZACCESS;
	goto done;
    }
    if (arestmode != 0 && arestmode != 1) {
	code = BZDOM;
	goto done;
    }
    bozo_SetRestricted(arestmode);
    code = WriteBozoFile(0);

 done:
    BNODE_UNLOCK();
    return code;
}

void *
bozo_ShutdownAndExit(void *param)
{
    int asignal = (intptr_t)param;
    int code;

    BNODE_LOCK();

    ViceLog(0,
	("Shutdown of BOS server and processes in response to signal %d\n",
	 asignal));

    /* start shutdown of all processes */
    if ((code = bnode_ApplyInstance(sdproc, NULL)) == 0) {
	/* wait for shutdown to complete */
	code = bnode_ApplyInstance(swproc, NULL);
    }

    if (code) {
	ViceLog(0, ("Shutdown incomplete (code = %d); manual cleanup required\n",
		    code));
    }

    BNODE_UNLOCK();

    rx_Finalize();
    exit(code);
}
