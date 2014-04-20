/*
Copyright 2011 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

#include "lz77.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void InitLZ77Store(LZ77Store* store) {
  store->size = 0;
  store->litlens = 0;
  store->dists = 0;
}

void CleanLZ77Store(LZ77Store* store) {
  free(store->litlens);
  free(store->dists);
}

void CopyLZ77Store(
    const LZ77Store* source, LZ77Store* dest) {
  size_t i;
  CleanLZ77Store(dest);
  dest->litlens =
      (unsigned short*)malloc(sizeof(*dest->litlens) * source->size);
  dest->dists = (unsigned short*)malloc(sizeof(*dest->dists) * source->size);

  if (!dest->litlens || !dest->dists) exit(-1); /* Allocation failed. */

  dest->size = source->size;
  for (i = 0; i < source->size; i++) {
    dest->litlens[i] = source->litlens[i];
    dest->dists[i] = source->dists[i];
  }
}

/*
Appends the length and distance to the LZ77 arrays of the LZ77Store.
context must be a LZ77Store*.
*/
void StoreLitLenDist(unsigned short length, unsigned short dist,
                     LZ77Store* store) {
  size_t size2 = store->size;  /* Needed for using APPEND_DATA twice. */
  APPEND_DATA(length, &store->litlens, &store->size);
  APPEND_DATA(dist, &store->dists, &size2);
}

/*
Gets the value of the length given the distance. Typically, the value of the
length is the length, but if the distance is very long, decrease the value of
the length a bit to make up for the fact that long distances use large amounts
of extra bits.
*/
static int GetLengthValue(int length, int distance) {
  /*
  At distance > 1024, using length 3 is no longer good, due to the large amount
  of extra bits for the distance code. distance > 1024 uses 9+ extra bits, and
  this seems to be the sweet spot.
  */
  return distance > 1024 ? length - 1 : length;
}

void VerifyLenDist(const unsigned char* data, size_t datasize, size_t pos,
                   unsigned short dist, unsigned short length) {

  /* TODO(lode): make this only run in a debug compile, it's for assert only. */
  size_t i;

  assert(pos + length <= datasize);
  for (i = 0; i < length; i++) {
    if (data[pos - dist + i] != data[pos + i]) {
      assert(data[pos - dist + i] == data[pos + i]);
      break;
    }
  }
}

/*
Finds how long the match of scan and match is. Can be used to find how many
bytes starting from scan, and from match, are equal. Returns the last byte
after scan, which is still equal to the correspondinb byte after match.
scan is the position to compare
match is the earlier position to compare.
end is the last possible byte, beyond which to stop looking.
safe_end is a few (8) bytes before end, for comparing multiple bytes at once.
*/
static const unsigned char* GetMatch(const unsigned char* scan,
                                     const unsigned char* match,
                                     const unsigned char* end,
                                     const unsigned char* safe_end) {

  if (sizeof(size_t) == 8) {
    /* 8 checks at once per array bounds check (size_t is 64-bit). */
    while (scan < safe_end && *((size_t*)scan) == *((size_t*)match)) {
      scan += 8;
      match += 8;
    }
  } else if (sizeof(unsigned int) == 4) {
    /* 4 checks at once per array bounds check (unsigned int is 32-bit). */
    while (scan < safe_end
        && *((unsigned int*)scan) == *((unsigned int*)match)) {
      scan += 4;
      match += 4;
    }
  } else {
    /* do 8 checks at once per array bounds check. */
    while (scan < safe_end && *scan == *match && *++scan == *++match
          && *++scan == *++match && *++scan == *++match
          && *++scan == *++match && *++scan == *++match
          && *++scan == *++match && *++scan == *++match) {
      scan++; match++;
    }
  }

  /* The remaining few bytes. */
  while (scan != end && *scan == *match) {
    scan++; match++;
  }

  return scan;
}

#ifdef USE_LONGEST_MATCH_CACHE
/*
Gets distance, length and sublen values from the cache if possible.
Returns 1 if it got the values from the cache, 0 if not.
Updates the limit value to a smaller one if possible with more limited
information from the cache.
*/
int TryGetFromLongestMatchCache(BlockState* s, size_t pos, size_t* limit,
    unsigned short* sublen, unsigned short* distance, unsigned short* length) {
  /* The LMC cache starts at the beginning of the block rather than the
     beginning of the whole array. */
  size_t lmcpos = pos - s->blockstart;

  /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
     that this cache value is not filled in yet. */
  unsigned char cache_available = s->lmc && (s->lmc->length[lmcpos] == 0 ||
      s->lmc->dist[lmcpos] != 0);
  unsigned char limit_ok_for_cache = cache_available && (*limit == MAX_MATCH ||
      s->lmc->length[lmcpos] <= *limit ||
      (sublen && MaxCachedSublen(s->lmc,
          lmcpos, s->lmc->length[lmcpos]) >= *limit));

  if (s->lmc && limit_ok_for_cache && cache_available) {
    if (!sublen || s->lmc->length[lmcpos]
        <= MaxCachedSublen(s->lmc, lmcpos, s->lmc->length[lmcpos])) {
      *length = s->lmc->length[lmcpos];
      if (*length > *limit) *length = *limit;
      if (sublen) {
        CacheToSublen(s->lmc, lmcpos, *length, sublen);
        *distance = sublen[*length];
        if (*limit == MAX_MATCH && *length >= MIN_MATCH) {
          assert(sublen[*length] == s->lmc->dist[lmcpos]);
        }
      } else {
        *distance = s->lmc->dist[lmcpos];
      }
      return 1;
    }
    /* Can't use much of the cache, since the "sublens" need to be calculated,
       but at  least we already know when to stop. */
    *limit = s->lmc->length[lmcpos];
  }

  return 0;
}

/*
Stores the found sublen, distance and length in the longest match cache, if
possible.
*/
void StoreInLongestMatchCache(BlockState* s, size_t pos, size_t limit,
    const unsigned short* sublen,
    unsigned short distance, unsigned short length) {
  /* The LMC cache starts at the beginning of the block rather than the
     beginning of the whole array. */
  size_t lmcpos = pos - s->blockstart;

  /* Length > 0 and dist 0 is invalid combination, which indicates on purpose
     that this cache value is not filled in yet. */
  unsigned char cache_available = s->lmc && (s->lmc->length[lmcpos] == 0 ||
      s->lmc->dist[lmcpos] != 0);

  if (s->lmc && limit == MAX_MATCH && sublen && !cache_available) {
    assert(s->lmc->length[lmcpos] == 1 && s->lmc->dist[lmcpos] == 0);
    s->lmc->dist[lmcpos] = length < MIN_MATCH ? 0 : distance;
    s->lmc->length[lmcpos] = length < MIN_MATCH ? 0 : length;
    assert(!(s->lmc->length[lmcpos] == 1 && s->lmc->dist[lmcpos] == 0));
    SublenToCache(sublen, lmcpos, length, s->lmc);
  }
}
#endif

void FindLongestMatch(BlockState* s, const Hash* h, const unsigned char* array,
    size_t pos, size_t size, size_t limit,
    unsigned short* sublen, unsigned short* distance, unsigned short* length) {
  unsigned short hpos = pos & WINDOW_MASK, p, pp;
  unsigned short bestdist = 0;
  unsigned short bestlength = 1;
  const unsigned char* scan;
  const unsigned char* match;
  const unsigned char* arrayend;
  const unsigned char* arrayend_safe;
#if MAX_CHAIN_HITS < WINDOW_SIZE
  int chain_counter = MAX_CHAIN_HITS;  /* For quitting early. */
#endif

  unsigned dist = 0;  /* Not unsigned short on purpose. */

  int* hhead = h->head;
  unsigned short* hprev = h->prev;
  int* hhashval = h->hashval;
  int hval = h->val;

#ifdef USE_LONGEST_MATCH_CACHE
  if (TryGetFromLongestMatchCache(s, pos, &limit, sublen, distance, length)) {
    assert(pos + *length <= size);
    return;
  }
#endif

  assert(limit <= MAX_MATCH);
  assert(limit >= MIN_MATCH);
  assert(pos < size);

  if (size - pos < MIN_MATCH) {
    /* The rest of the code assumes there are at least MIN_MATCH bytes to
       try. */
    *length = 0;
    *distance = 0;
    return;
  }

  if (pos + limit > size) {
    limit = size - pos;
  }
  arrayend = &array[pos] + limit;
  arrayend_safe = arrayend - 8;

  assert(hval < 65536);

  pp = hhead[hval];  /* During the whole loop, p == hprev[pp]. */
  p = hprev[pp];

  assert(pp == hpos);

  dist = p < pp ? pp - p : ((WINDOW_SIZE - p) + pp);

  /* Go through all distances. */
  while (dist < WINDOW_SIZE) {
    unsigned short currentlength = 0;

    assert(p < WINDOW_SIZE);
    assert(p == hprev[pp]);
    assert(hhashval[p] == hval);

    if (dist > 0) {
      assert(pos < size);
      assert(dist <= pos);
      scan = &array[pos];
      match = &array[pos - dist];

      /* Testing the byte at position bestlength first, goes slightly faster. */
      if (pos + bestlength >= size
          || *(scan + bestlength) == *(match + bestlength)) {

#ifdef USE_HASH_SAME
        unsigned short same0 = h->same[pos & WINDOW_MASK];
        if (same0 > 2 && *scan == *match) {
          unsigned short same1 = h->same[(pos - dist) & WINDOW_MASK];
          unsigned short same = same0 < same1 ? same0 : same1;
          if (same > limit) same = limit;
          scan += same;
          match += same;
        }
#endif
        scan = GetMatch(scan, match, arrayend, arrayend_safe);
        currentlength = scan - &array[pos];  /* The found length. */
      }

      if (currentlength > bestlength) {
        if (sublen) {
          unsigned short j;
          for (j = bestlength + 1; j <= currentlength; j++) {
            sublen[j] = dist;
          }
        }
        bestdist = dist;
        bestlength = currentlength;
        if (currentlength >= limit) break;
      }
    }


#ifdef USE_HASH_SAME_HASH
    /* Switch to the other hash once this will be more efficient. */
    if (hhead != h->head2 && bestlength >= h->same[hpos] &&
        h->val2 == h->hashval2[p]) {
      /* Now use the hash that encodes the length and first byte. */
      hhead = h->head2;
      hprev = h->prev2;
      hhashval = h->hashval2;
      hval = h->val2;
    }
#endif

    pp = p;
    p = hprev[p];
    if (p == pp) break;  /* Uninited prev value. */

    dist += p < pp ? pp - p : ((WINDOW_SIZE - p) + pp);

#if MAX_CHAIN_HITS < WINDOW_SIZE
    chain_counter--;
    if (chain_counter <= 0) break;
#endif
  }

#ifdef USE_LONGEST_MATCH_CACHE
  StoreInLongestMatchCache(s, pos, limit, sublen, bestdist, bestlength);
#endif

  assert(bestlength <= limit);

  *distance = bestdist;
  *length = bestlength;
  assert(pos + *length <= size);
}

void LZ77Greedy(BlockState* s, const unsigned char* in,
                size_t instart, size_t inend,
                LZ77Store* store) {
  size_t i = 0, j;
  unsigned short leng;
  unsigned short dist;
  int lengvalue;
  size_t windowstart = instart > WINDOW_SIZE ? instart - WINDOW_SIZE : 0;
  unsigned short dummysublen[259];

  Hash hash;
  Hash* h = &hash;

#ifdef LAZY_MATCHING
  /* Lazy matching. */
  unsigned prev_length = 0;
  unsigned prev_match = 0;
  int prevlengvalue;
  int match_available = 0;
#endif

  if (instart == inend) return;

  InitHash(WINDOW_SIZE, h);
  WarmupHash(in, windowstart, inend, h);
  for (i = windowstart; i < instart; i++) {
    UpdateHash(in, i, inend, h);
  }

  for (i = instart; i < inend; i++) {
    UpdateHash(in, i, inend, h);

    FindLongestMatch(s, h, in, i, inend, MAX_MATCH, dummysublen, &dist, &leng);
    lengvalue = GetLengthValue(leng, dist);

#ifdef LAZY_MATCHING
    /* Lazy matching. */
    prevlengvalue = GetLengthValue(prev_length, prev_match);
    if (match_available) {
      match_available = 0;
      if (lengvalue > prevlengvalue + 1) {
        StoreLitLenDist(in[i - 1], 0, store);
        if (lengvalue >= MIN_MATCH && lengvalue < MAX_MATCH) {
          match_available = 1;
          prev_length = leng;
          prev_match = dist;
          continue;
        }
      } else {
        /* Add previous to output. */
        leng = prev_length;
        dist = prev_match;
        lengvalue = prevlengvalue;
        /* Add to output. */
        VerifyLenDist(in, inend, i - 1, dist, leng);
        StoreLitLenDist(leng, dist, store);
        for (j = 2; j < leng; j++) {
          assert(i < inend);
          i++;
          UpdateHash(in, i, inend, h);
        }
        continue;
      }
    }
    else if (lengvalue >= MIN_MATCH && leng < MAX_MATCH) {
      match_available = 1;
      prev_length = leng;
      prev_match = dist;
      continue;
    }
    /* End of lazy matching. */
#endif

    /* Add to output. */
    if (lengvalue >= MIN_MATCH) {
      VerifyLenDist(in, inend, i, dist, leng);
      StoreLitLenDist(leng, dist, store);
    } else {
      leng = 1;
      StoreLitLenDist(in[i], 0, store);
    }
    for (j = 1; j < leng; j++) {
      assert(i < inend);
      i++;
      UpdateHash(in, i, inend, h);
    }
  }

  CleanHash(h);
}

void GetLZ77Counts(const unsigned short* litlens, const unsigned short* dists,
                   size_t start, size_t end,
                   size_t* ll_count, size_t* d_count) {
  size_t i;

  for (i = 0; i < 288; i++) {
    ll_count[i] = 0;
  }
  for (i = 0; i < 32; i++) {
    d_count[i] = 0;
  }

  for (i = start; i < end; i++) {
    if (dists[i] == 0) {
      ll_count[litlens[i]]++;
    } else {
      ll_count[GetLengthSymbol(litlens[i])]++;
      d_count[GetDistSymbol(dists[i])]++;
    }
  }

  ll_count[256] = 1;  /* End symbol. */
}
