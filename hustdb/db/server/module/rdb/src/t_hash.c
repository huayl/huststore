/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "../lib/server.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * ziplist to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion ( robj *o, robj **argv, int start, int end )
{
    int i;

    if ( o->encoding != OBJ_ENCODING_ZIPLIST ) return;

    for ( i = start; i <= end; i ++ )
    {
        if ( sdsEncodedObject (argv[i]) &&
             sdslen (argv[i]->ptr) > server.hash_max_ziplist_value )
        {
            hashTypeConvert (o, OBJ_ENCODING_HT);
            break;
        }
    }
}

/* Encode given objects in-place when the hash uses a dict. */
void hashTypeTryObjectEncoding ( robj *subject, robj **o1, robj **o2 )
{
    if ( subject->encoding == OBJ_ENCODING_HT )
    {
        if ( o1 ) *o1 = tryObjectEncoding (*o1);
        if ( o2 ) *o2 = tryObjectEncoding (*o2);
    }
}

/* Get the value from a ziplist encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
int hashTypeGetFromZiplist ( robj *o, robj *field,
                             unsigned char **vstr,
                             unsigned int *vlen,
                             long long *vll )
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;
    int ret;

    field = getDecodedObject (field);

    zl = o->ptr;
    fptr = ziplistIndex (zl, ZIPLIST_HEAD);
    if ( fptr != NULL )
    {
        fptr = ziplistFind (fptr, field->ptr, sdslen (field->ptr), 1);
        if ( fptr != NULL )
        {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = ziplistNext (zl, fptr);
        }
    }

    decrRefCount (field);

    if ( vptr != NULL )
    {
        ret = ziplistGet (vptr, vstr, vlen, vll);
        return 0;
    }

    return - 1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
int hashTypeGetFromHashTable ( robj *o, robj *field, robj **value )
{
    dictEntry *de;

    de = dictFind (o->ptr, field);
    if ( de == NULL ) return - 1;
    *value = dictGetVal (de);
    return 0;
}

/* Higher level function of hashTypeGet*() that always returns a Redis
 * object (either new or with refcount incremented), so that the caller
 * can retain a reference or call decrRefCount after the usage.
 *
 * The lower level function can prevent copy on write so it is
 * the preferred way of doing read operations. */
robj *hashTypeGetObject ( robj *o, robj *field )
{
    robj *value = NULL;

    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if ( hashTypeGetFromZiplist (o, field, &vstr, &vlen, &vll) == 0 )
        {
            if ( vstr )
            {
                value = createStringObject (( char* ) vstr, vlen);
            }
            else
            {
                value = createStringObjectFromLongLong (vll);
            }
        }
    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        robj *aux;

        if ( hashTypeGetFromHashTable (o, field, &aux) == 0 )
        {
            incrRefCount (aux);
            value = aux;
        }
    }
    else
    {
        return NULL;
    }
    return value;
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
size_t hashTypeGetValueLength ( robj *o, robj *field )
{
    size_t len = 0;
    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if ( hashTypeGetFromZiplist (o, field, &vstr, &vlen, &vll) == 0 )
            len = vstr ? vlen : sdigits10 (vll);
    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        robj *aux;

        if ( hashTypeGetFromHashTable (o, field, &aux) == 0 )
            len = stringObjectLen (aux);
    }
    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
int hashTypeExists ( robj *o, robj *field )
{
    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if ( hashTypeGetFromZiplist (o, field, &vstr, &vlen, &vll) == 0 ) return 1;
    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        robj *aux;

        if ( hashTypeGetFromHashTable (o, field, &aux) == 0 ) return 1;
    }
    return 0;
}

/* Add an element, discard the old if the key already exists.
 * Return 0 on insert and 1 on update.
 * This function will take care of incrementing the reference count of the
 * retained fields and value objects. */
int hashTypeSet ( robj *o, robj *field, robj *value )
{
    int update = 0;

    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *zl, *fptr, *vptr;

        field = getDecodedObject (field);
        value = getDecodedObject (value);

        zl = o->ptr;
        fptr = ziplistIndex (zl, ZIPLIST_HEAD);
        if ( fptr != NULL )
        {
            fptr = ziplistFind (fptr, field->ptr, sdslen (field->ptr), 1);
            if ( fptr != NULL )
            {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = ziplistNext (zl, fptr);
                update = 1;

                /* Delete value */
                zl = ziplistDelete (zl, &vptr);

                /* Insert new value */
                zl = ziplistInsert (zl, vptr, value->ptr, sdslen (value->ptr));
            }
        }

        if ( ! update )
        {
            /* Push new field/value pair onto the tail of the ziplist */
            zl = ziplistPush (zl, field->ptr, sdslen (field->ptr), ZIPLIST_TAIL);
            zl = ziplistPush (zl, value->ptr, sdslen (value->ptr), ZIPLIST_TAIL);
        }
        o->ptr = zl;
        decrRefCount (field);
        decrRefCount (value);

        /* Check if the ziplist needs to be converted to a hash table */
        if ( hashTypeLength (o) > server.hash_max_ziplist_entries )
            hashTypeConvert (o, OBJ_ENCODING_HT);
    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        if ( dictReplace (o->ptr, field, value) )
        { /* Insert */
            incrRefCount (field);
        }
        else
        { /* Update */
            update = 1;
        }
        incrRefCount (value);
    }
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete ( robj *o, robj *field )
{
    int deleted = 0;

    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *zl, *fptr;

        field = getDecodedObject (field);

        zl = o->ptr;
        fptr = ziplistIndex (zl, ZIPLIST_HEAD);
        if ( fptr != NULL )
        {
            fptr = ziplistFind (fptr, field->ptr, sdslen (field->ptr), 1);
            if ( fptr != NULL )
            {
                zl = ziplistDelete (zl, &fptr);
                zl = ziplistDelete (zl, &fptr);
                o->ptr = zl;
                deleted = 1;
            }
        }

        decrRefCount (field);

    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        if ( dictDelete (( dict* ) o->ptr, field) == C_OK )
        {
            deleted = 1;

            /* Always check if the dictionary needs a resize after a delete. */
            if ( htNeedsResize (o->ptr) ) dictResize (o->ptr);
        }

    }

    return deleted;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength ( robj *o )
{
    unsigned long length = ULONG_MAX;

    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        length = ziplistLen (o->ptr) / 2;
    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        length = dictSize (( dict* ) o->ptr);
    }

    return length;
}

hashTypeIterator *hashTypeInitIterator ( robj *subject )
{
    hashTypeIterator *hi = zmalloc (sizeof (hashTypeIterator ));
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if ( hi->encoding == OBJ_ENCODING_ZIPLIST )
    {
        hi->fptr = NULL;
        hi->vptr = NULL;
    }
    else if ( hi->encoding == OBJ_ENCODING_HT )
    {
        hi->di = dictGetIterator (subject->ptr);
    }

    return hi;
}

void hashTypeReleaseIterator ( hashTypeIterator *hi )
{
    if ( hi->encoding == OBJ_ENCODING_HT )
    {
        dictReleaseIterator (hi->di);
    }

    zfree (hi);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext ( hashTypeIterator *hi )
{
    if ( hi->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if ( fptr == NULL )
        {
            /* Initialize cursor */
            fptr = ziplistIndex (zl, 0);
        }
        else
        {
            /* Advance cursor */
            fptr = ziplistNext (zl, vptr);
        }
        if ( fptr == NULL ) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = ziplistNext (zl, fptr);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    }
    else if ( hi->encoding == OBJ_ENCODING_HT )
    {
        if ( ( hi->de = dictNext (hi->di) ) == NULL ) return C_ERR;
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a ziplist. Prototype is similar to `hashTypeGetFromZiplist`. */
void hashTypeCurrentFromZiplist ( hashTypeIterator *hi, int what,
                                  unsigned char **vstr,
                                  unsigned int *vlen,
                                  long long *vll )
{
    int ret;


    if ( what & OBJ_HASH_KEY )
    {
        ret = ziplistGet (hi->fptr, vstr, vlen, vll);
    }
    else
    {
        ret = ziplistGet (hi->vptr, vstr, vlen, vll);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a ziplist. Prototype is similar to `hashTypeGetFromHashTable`. */
void hashTypeCurrentFromHashTable ( hashTypeIterator *hi, int what, robj **dst )
{

    if ( what & OBJ_HASH_KEY )
    {
        *dst = dictGetKey (hi->de);
    }
    else
    {
        *dst = dictGetVal (hi->de);
    }
}

/* A non copy-on-write friendly but higher level version of hashTypeCurrent*()
 * that returns an object with incremented refcount (or a new object). It is up
 * to the caller to decrRefCount() the object if no reference is retained. */
robj *hashTypeCurrentObject ( hashTypeIterator *hi, int what )
{
    robj *dst;

    if ( hi->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist (hi, what, &vstr, &vlen, &vll);
        if ( vstr )
        {
            dst = createStringObject (( char* ) vstr, vlen);
        }
        else
        {
            dst = createStringObjectFromLongLong (vll);
        }
    }
    else if ( hi->encoding == OBJ_ENCODING_HT )
    {
        hashTypeCurrentFromHashTable (hi, what, &dst);
        incrRefCount (dst);
    }
    return dst;
}

robj *hashTypeLookupWriteOrCreate ( client *c, robj *key )
{
    robj *o = lookupKeyWrite (c->db, key);
    if ( o == NULL )
    {
        o = createHashObject ();
        dbAdd (c->db, key, o);
    }
    else
    {
        if ( o->type != OBJ_HASH )
        {
            return NULL;
        }
    }
    return o;
}

void hashTypeConvertZiplist ( robj *o, int enc )
{

    if ( enc == OBJ_ENCODING_ZIPLIST )
    {
        /* Nothing to do... */

    }
    else if ( enc == OBJ_ENCODING_HT )
    {
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        hi = hashTypeInitIterator (o);
        dict = dictCreate (&hashDictType, NULL);

        while ( hashTypeNext (hi) != C_ERR )
        {
            robj *field, *value;

            field = hashTypeCurrentObject (hi, OBJ_HASH_KEY);
            field = tryObjectEncoding (field);
            value = hashTypeCurrentObject (hi, OBJ_HASH_VALUE);
            value = tryObjectEncoding (value);
            ret = dictAdd (dict, field, value);
        }

        hashTypeReleaseIterator (hi);
        zfree (o->ptr);

        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;

    }
}

void hashTypeConvert ( robj *o, int enc )
{
    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        hashTypeConvertZiplist (o, enc);
    }
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

int hsetCommand ( client *c )
{
    int update;
    robj *o;

    if ( ( o = hashTypeLookupWriteOrCreate (c, c->argv[1]) ) == NULL ) return C_ERR;
    hashTypeTryConversion (o, c->argv, 2, 3);
    hashTypeTryObjectEncoding (o, &c->argv[2], &c->argv[3]);
    update = hashTypeSet (o, c->argv[2], c->argv[3]);
    return C_OK;
}

int hsetnxCommand ( client *c )
{
    robj *o;
    if ( ( o = hashTypeLookupWriteOrCreate (c, c->argv[1]) ) == NULL ) return C_ERR;
    hashTypeTryConversion (o, c->argv, 2, 3);

    if ( hashTypeExists (o, c->argv[2]) )
    {
        return C_ERR;
    }
    else
    {
        hashTypeTryObjectEncoding (o, &c->argv[2], &c->argv[3]);
        hashTypeSet (o, c->argv[2], c->argv[3]);
    }
    return C_OK;
}

int hmsetCommand ( client *c )
{
    int i;
    robj *o;

    if ( ( c->argc % 2 ) == 1 )
    {
        return C_ERR;
    }

    if ( ( o = hashTypeLookupWriteOrCreate (c, c->argv[1]) ) == NULL ) return C_ERR;
    hashTypeTryConversion (o, c->argv, 2, c->argc - 1);
    for ( i = 2; i < c->argc; i += 2 )
    {
        hashTypeTryObjectEncoding (o, &c->argv[i], &c->argv[i + 1]);
        hashTypeSet (o, c->argv[i], c->argv[i + 1]);
    }
    return C_OK;
}

int hincrbyCommand ( client *c )
{
    long long value, incr, oldvalue;
    robj *o, *current, *new;

    if ( getLongLongFromObject (c->argv[3], &incr) != C_OK ) return C_ERR;
    if ( ( o = hashTypeLookupWriteOrCreate (c, c->argv[1]) ) == NULL ) return C_ERR;
    if ( ( current = hashTypeGetObject (o, c->argv[2]) ) != NULL )
    {
        if ( getLongLongFromObject (current, &value) != C_OK )
        {
            decrRefCount (current);
            return C_ERR;
        }
        decrRefCount (current);
    }
    else
    {
        value = 0;
    }

    oldvalue = value;
    if ( ( incr < 0 && oldvalue < 0 && incr < ( LLONG_MIN - oldvalue ) ) ||
         ( incr > 0 && oldvalue > 0 && incr > ( LLONG_MAX - oldvalue ) ) )
    {
        return C_ERR;
    }
    value += incr;
    new = createStringObjectFromLongLong (value);
    hashTypeTryObjectEncoding (o, &c->argv[2], NULL);
    hashTypeSet (o, c->argv[2], new);
    decrRefCount (new);
    addReplyLongLong (c, value);
    return C_OK;
}

int hincrbyfloatCommand ( client *c )
{
    double long value, incr;
    robj *o, *current, *new;

    if ( getLongDoubleFromObject (c->argv[3], &incr) != C_OK ) return C_ERR;
    if ( ( o = hashTypeLookupWriteOrCreate (c, c->argv[1]) ) == NULL ) return C_ERR;
    if ( ( current = hashTypeGetObject (o, c->argv[2]) ) != NULL )
    {
        if ( getLongDoubleFromObject (current, &value) != C_OK )
        {
            decrRefCount (current);
            return C_ERR;
        }
        decrRefCount (current);
    }
    else
    {
        value = 0;
    }

    value += incr;
    new = createStringObjectFromLongDouble (value, 1);
    hashTypeTryObjectEncoding (o, &c->argv[2], NULL);
    hashTypeSet (o, c->argv[2], new);
    addReply (c, new);

    return C_OK;
}

static void addHashFieldToReply ( client *c, robj *o, robj *field )
{
    int ret;

    if ( o == NULL )
    {
        return;
    }

    if ( o->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        ret = hashTypeGetFromZiplist (o, field, &vstr, &vlen, &vll);
        if ( ret < 0 )
        {
            addReplyNULL (c);
            return;
        }
        else
        {
            if ( vstr )
            {
                addReplyString (c, ( char * ) vstr, vlen);
            }
            else
            {
                addReplyLongLong (c, vll);
            }
        }

    }
    else if ( o->encoding == OBJ_ENCODING_HT )
    {
        robj *value;

        ret = hashTypeGetFromHashTable (o, field, &value);
        if ( ret < 0 )
        {
            addReplyNULL (c);
            return;
        }
        else
        {
            addReply (c, value);
        }

    }
}

int hgetCommand ( client *c )
{
    robj *o;

    if ( ( o = lookupKeyRead (c->db, c->argv[1]) ) == NULL ||
         checkType (c, o, OBJ_HASH) )
    {
        return C_ERR;
    }

    addHashFieldToReply (c, o, c->argv[2]);
    return C_OK;
}

int hmgetCommand ( client *c )
{
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    o = lookupKeyRead (c->db, c->argv[1]);
    if ( o != NULL && o->type != OBJ_HASH )
    {
        return C_ERR;
    }

    for ( i = 2; i < c->argc; i ++ )
    {
        addHashFieldToReply (c, o, c->argv[i]);
    }
    return C_OK;
}

int hdelCommand ( client *c )
{
    robj *o;
    int j, deleted = 0;

    if ( ( o = lookupKeyWrite (c->db, c->argv[1]) ) == NULL ||
         checkType (c, o, OBJ_HASH) ) return C_ERR;

    for ( j = 2; j < c->argc; j ++ )
    {
        if ( hashTypeDelete (o, c->argv[j]) )
        {
            deleted ++;
            if ( hashTypeLength (o) == 0 )
            {
                dbDelete (c->db, c->argv[1]);
                break;
            }
        }
    }

    addReplyLongLong (c, deleted);

    return C_OK;
}

int hlenCommand ( client *c )
{
    robj *o;

    if ( ( o = lookupKeyRead (c->db, c->argv[1]) ) == NULL ||
         checkType (c, o, OBJ_HASH) ) return C_ERR;

    addReplyLongLong (c, hashTypeLength (o));

    return C_OK;
}

int hstrlenCommand ( client *c )
{
    robj *o;

    if ( ( o = lookupKeyRead (c->db, c->argv[1]) ) == NULL ||
         checkType (c, o, OBJ_HASH) ) return C_ERR;
    addReplyLongLong (c, hashTypeGetValueLength (o, c->argv[2]));
    return C_OK;
}

static void addHashIteratorCursorToReply ( client *c, hashTypeIterator *hi, int what )
{
    if ( hi->encoding == OBJ_ENCODING_ZIPLIST )
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist (hi, what, &vstr, &vlen, &vll);
        if ( vstr )
        {
            addReplyString (c, ( char * ) vstr, vlen);
        }
        else
        {
            addReplyLongLong (c, vll);
        }

    }
    else if ( hi->encoding == OBJ_ENCODING_HT )
    {
        robj *value;

        hashTypeCurrentFromHashTable (hi, what, &value);
        addReply (c, value);

    }
}

int genericHgetallCommand ( client *c, int flags )
{
    robj *o;
    hashTypeIterator *hi;
    int multiplier = 0;
    int length, count = 0;

    if ( ( o = lookupKeyRead (c->db, c->argv[1]) ) == NULL
         || checkType (c, o, OBJ_HASH) ) return C_ERR;

    if ( flags & OBJ_HASH_KEY ) multiplier ++;
    if ( flags & OBJ_HASH_VALUE ) multiplier ++;

    length = hashTypeLength (o) * multiplier;

    hi = hashTypeInitIterator (o);
    while ( hashTypeNext (hi) != C_ERR )
    {
        if ( flags & OBJ_HASH_KEY )
        {
            addHashIteratorCursorToReply (c, hi, OBJ_HASH_KEY);
            count ++;
        }
        if ( flags & OBJ_HASH_VALUE )
        {
            addHashIteratorCursorToReply (c, hi, OBJ_HASH_VALUE);
            count ++;
        }
    }

    hashTypeReleaseIterator (hi);
    return C_OK;
}

int hkeysCommand ( client *c )
{
    return genericHgetallCommand (c, OBJ_HASH_KEY);
}

int hvalsCommand ( client *c )
{
    return genericHgetallCommand (c, OBJ_HASH_VALUE);
}

int hgetallCommand ( client *c )
{
    return genericHgetallCommand (c, OBJ_HASH_KEY | OBJ_HASH_VALUE);
}

int hexistsCommand ( client *c )
{
    robj *o;
    if ( ( o = lookupKeyRead (c->db, c->argv[1]) ) == NULL ||
         checkType (c, o, OBJ_HASH) ) return C_ERR;

    addReplyLongLong (c, hashTypeExists (o, c->argv[2]) ? 1 : 0);
    return C_OK;
}

int hscanCommand ( client *c )
{
    robj *o;
    unsigned long cursor;

    if ( parseScanCursor (c, c->argv[2], &cursor) == C_ERR ) return C_ERR;
    if ( ( o = lookupKeyRead (c->db, c->argv[1]) ) == NULL ||
         checkType (c, o, OBJ_HASH) ) return C_ERR;
    scanGenericCommand (c, o, cursor);
    return C_OK;
}