#include <immintrin.h>

#define	HASHSZ          30383		// Emperically derived optimum
#define MAX_READERS         8   	// Virtual systems don't like too many readers
#define	MAX_SOLUTIONS    8192
#define	MAX_WORDS        8192
#define	MAX_THREADS        14

static const char	*solution_filename = "solutions.txt";

// Worker thread state
static struct worker {
	char     *start;
	char     *end;
} workers[MAX_THREADS] __attribute__ ((aligned(64)));

// Set Pointers (24 bytes in size)
struct tier {
	uint32_t	*s;	// Pointer to set
	uint32_t	l;	// Length of set
	uint32_t	toff1;	// Tiered Offset 1
	uint32_t	toff2;	// Tiered Offset 2
	uint32_t	toff3;	// Tiered Offset 3
};

// Character frequency recording
static struct frequency {
	uint32_t	m;		// Mask (1 << (c - 'a'))
	int32_t		f;		// Frequency
	uint32_t	tm1;		// Tiered Mask 1
	uint32_t	tm2;		// Tiered Mask 2
	uint32_t	tm3;		// Tiered Mask 3
	uint32_t	tm4;		// Tiered Mask 4
	uint32_t	tm5;		// Tiered Mask 5
	uint32_t	tm6;		// Tiered Mask 6
	int		ready_to_setup;	//
	int		b;

	// Position within a set
	atomic_int	pos __attribute__((aligned(64)));

	// Tier Sets (384 bytes)
	struct tier	sets[16] __attribute__((aligned(64)));
} frq[26] __attribute__ ((aligned(64)));

// Keep atomic variables on their own CPU cache line
atomic_int 	num_words	__attribute__ ((aligned(64))) = 0;
atomic_int	file_pos	__attribute__ ((aligned(64))) = 0;
atomic_int	num_sol		__attribute__ ((aligned(64))) = 0;
atomic_int	setup_set	__attribute__ ((aligned(64))) = 0;
atomic_int	setups_done	__attribute__ ((aligned(64))) = 0;
atomic_int	readers_done	__attribute__ ((aligned(64))) = 0;
atomic_int	solvers_done	__attribute__ ((aligned(64))) = 0;
atomic_int	first_rdr_done	__attribute__ ((aligned(64))) = 0;

// Put volatile thread sync variables on their own CPU cache line
static volatile int	workers_start	__attribute__ ((aligned(64))) = 0;
static volatile int	go_solve	__attribute__ ((aligned(64))) = 0;
static volatile int	num_readers	__attribute__ ((aligned(64))) = 0;

// Put general global variables on their own CPU cache line
static int32_t	min_search_depth __attribute__ ((aligned(64))) = 0;
static uint32_t hash_collisions = 0;
static int	write_metrics = 0;
static int	nthreads = 0;
static int	nkeys = 0;

// We build the solutions directly as a character array to write out when done
static char     solutions[MAX_SOLUTIONS * 30] __attribute__ ((aligned(64)));

// Key Hash Entries
// We keep keys and positions in separate array because faster to initialise
uint32_t keymap[HASHSZ] __attribute__ ((aligned(64)));
uint32_t posmap[HASHSZ] __attribute__ ((aligned(64)));

// Allow for up to 3x the number of unique non-anagram words
static char     words[MAX_WORDS * 24] __attribute__ ((aligned(64)));
static uint32_t wordkeys[MAX_WORDS * 3] __attribute__ ((aligned(64)));

// We add 1024 here to MAX_WORDS to give us extra space to perform vector
// alignments for the AVX functions.  At the very least the keys array must
// be 32-byte aligned, but we align it to 64 bytes anyway
static	uint32_t	keys[MAX_WORDS + 1024] __attribute__ ((aligned(64)));
static	uint32_t	tkeys[26][MAX_WORDS * 2] __attribute__ ((aligned(64)));
static	uint32_t	unmap[32] __attribute__((aligned(64)));

static void solve();
static void solve_work();
static void set_tier_offsets(struct frequency *f);

void
print_time_taken(char *label, struct timespec *ts, struct timespec *te)
{
	int64_t time_taken = 1000000000LL;	// Number of ns in 1s
	time_taken *= (te->tv_sec - ts->tv_sec);
	time_taken += (te->tv_nsec - ts->tv_nsec);

	printf("%-20s = %ld.%06lus\n", label, time_taken / 1000000000, (time_taken % 1000000000) / 1000);
} // print_time_taken
 
//********************* INIT FUNCTIONS **********************

static void
frq_init()
{
	memset(frq, 0, sizeof(frq));

	for (int b = 0; b < 26; b++)
		frq[b].m = (1UL << b);	// The bit mask
} // frq_init

static void
hash_init()
{
	memset(keymap, 0, sizeof(keymap));
} // hash_init


//********************* UTILITY FUNCTIONS **********************

// Determine number of threads to use
int
get_nthreads()
{
	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	if (ncpus < 2)
		return 1;

	if (ncpus < 5)
		return ncpus;

	if (ncpus < 9)
		return ncpus - 1;

	// Generally speaking, not much to be gained beyond 20 threads
	if ((ncpus - 2) > 20)
		return 20;

	return ncpus - 2;
} // get_nthreads

// Given a 5 letter word, calculate the bit-map representation of that word
static inline uint32_t
calc_key(const char *wd)
{
	uint32_t one = 1, a = 'a';
	uint32_t key;

	key  = (one << (*wd++ - a));
	key |= (one << (*wd++ - a));
	key |= (one << (*wd++ - a));
	key |= (one << (*wd++ - a));
	key |= (one << (*wd   - a));

	return key;
} // calc_key

//********************* HASH TABLE FUNCTIONS **********************

// A very simple for-purpose hash map implementation.  Used to
// lookup words given the key representation of that word
#define key_hash(x)	(x % HASHSZ)
uint32_t
hash_insert(uint32_t key, uint32_t pos)
{
	uint32_t col = 0, hashpos = key_hash(key);

	do {
		// Check if we can insert at this position
		if (keymap[hashpos] == 0)
			break;

		// Check if duplicate key
		if (keymap[hashpos] == key)
			return 0;

		// Handle full hash table condition
		if (++col == HASHSZ)
			return 0;

		if (++hashpos == HASHSZ)
			hashpos -= HASHSZ;
	} while (1);

	// Now insert at hash location
	keymap[hashpos] = key;
	posmap[hashpos] = pos << 3;

	hash_collisions += col;

	return key;
} // hash_insert

const char *
hash_lookup(uint32_t key, const char *wp)
{
	uint32_t col = 0, hashpos = key_hash(key);

	do {
		// Check for a match
		if (keymap[hashpos] == key)
			break;

		// Check the not-in-hash scenario
		if (keymap[hashpos] == 0)
			return NULL;

		// Handle full hash table condition
		if (++col == HASHSZ)
			return NULL;

		if (++hashpos == HASHSZ)
			hashpos -= HASHSZ;
	} while (1);

	hash_collisions += col;

	return wp + posmap[hashpos];
} // hash_lookup
#undef key_hash

// Just a handy debugging function which was used when developing the
// 5 letter word extraction bit masking algorithm within find_words()
void
print_bits(char *label, uint64_t v)
{
	printf("%s ", label);
	for (int i = 0; i < 64; i++) {
		if (v >> 63)
			printf("1");
		else
			printf("0");
		v <<= 1;
	}
	printf("\n");
} // print_bits

// ********************* FILE READER ********************

#define READ_CHUNK        65536		// Appears to be optimum

void
find_words(char *s, char *e, uint32_t rn)
{
	char a = 'a', z = 'z', nl = '\n';
	char *fives[READ_CHUNK / 5];
	char **fivep = fives;

#ifdef __AVX2__
	// AVX512 is about 10% faster than AVX2 for processing the words
	// Use AVX512 if the current platform supports it

	// Prepare 3 constant vectors with newlines, a's and z's
#ifdef __AVX512F__
	__m512i nvec = _mm512_set1_epi8(nl);
	__m512i avec = _mm512_set1_epi8(a);
	__m512i zvec = _mm512_set1_epi8(z);
#else
	__m256i nvec = _mm256_set1_epi8(nl);
	__m256i avec = _mm256_set1_epi8(a);
	__m256i zvec = _mm256_set1_epi8(z);
#endif

	e -= 64;
	while (s < e) {
#ifdef __AVX512F__
		// Unaligned load of a vector with the next 64 characters
		__m512i wvec = _mm512_loadu_si512((const __m512i_u *)s);

		// Find the newlines in the word vector
		// nmask will have a 1-bit for every newline in the vector
		uint64_t nmask = _mm512_cmpeq_epi8_mask(nvec, wvec);

		// Find the lower-case letters in the word vector
		// wmask will have a 0-bit for every lower-case letter in the vector
		uint64_t wmask = _mm512_cmp_epi8_mask(wvec, avec, _MM_CMPINT_LT) |
					  _mm512_cmp_epi8_mask(zvec, wvec, _MM_CMPINT_LT);
#else
		// Here we're in AVX2 mode.  Emulate AVX512 mode by doing 2 loads

		// Unaligned load of 2 vectors with the next 64 characters
		__m256i wvec1 = _mm256_loadu_si256((const __m256i_u *)s);
		__m256i wvec2 = _mm256_loadu_si256((const __m256i_u *)(s + 32));

		// Find the newlines in the word vector
		// nmask will have a 1-bit for every newline in the vector
		__m256i nres = _mm256_cmpeq_epi8(nvec, wvec1);
		uint32_t nmask1 = _mm256_movemask_epi8(nres);

		// Find the lower-case letters in the word vector
		// wmask will have a 0-bit for every lower-case letter in the vector
		__m256i wres = _mm256_or_si256(_mm256_cmpgt_epi8(avec, wvec1),
						_mm256_cmpgt_epi8(wvec1, zvec));
		uint32_t wmask1 = _mm256_movemask_epi8(wres);

		// Load and process another 32 characters
		nres = _mm256_cmpeq_epi8(nvec, wvec2);
		uint64_t nmask = _mm256_movemask_epi8(nres);
		wres = _mm256_or_si256(_mm256_cmpgt_epi8(avec, wvec2),
					_mm256_cmpgt_epi8(wvec2, zvec));
		uint64_t wmask = _mm256_movemask_epi8(wres);

		// Merge the results of the two loads
		wmask = (wmask << 32) | wmask1;
		nmask = (nmask << 32) | nmask1;
#endif
		// Handle lines over 64 characters in length.  Jump ahead just
		// far enough such that we won't accidentally feed the last 5
		// characters from an overly long line into the next pass
		// !nmask is never true for words_alpha.txt, so the CPU branch
		// predictor should never get this wrong
		if (!nmask) {
			s += 58;
			continue;
		}

		// Calculate where to start the next loop pass
		int nextpos = 63 - __builtin_clzll(nmask);

		// Invalidate everything after the last newline
		wmask |= ~(0ULL) << nextpos++;

		// Get 1's complement of wmask
		uint64_t ocwm = ~wmask;

		// Isolate all words of 5 characters or less
		uint64_t five_or_less = ocwm & (wmask >> 5) & ((wmask << 1) | 1);

		// Prune words with less than 5 characters
		uint64_t not_less_than_five = ((ocwm & (ocwm >> 1)) >> 2) >> 1;

		// Intersect five_or_less with not_less_than_five
		wmask = five_or_less & not_less_than_five;

		// wmask will now contain a 1 bit located at the
		// start of every word with exactly 5 letters

		// Process all 5 letter words in the vector
		while (wmask) {
			// Get a pointer to the start of the 5 letter word
			char *w = s + __builtin_ctzll(wmask);

			// Add word to our list
			*fivep = w;

			// Advance list if word has no duplicate characters
			fivep += (__builtin_popcount(calc_key(w)) == 5);

			// Unset the lowest bit
			wmask &= (wmask - 1);
		}
		s += nextpos;
	}
	e += 64;
#endif

	// Scalar code to find 5 words. This also
	// handles residuals from the vector loop
	for (char c, *w = s; s < e; w = s) {
		c = *s++; if ((c < a) || (c > z)) continue;
		c = *s++; if ((c < a) || (c > z)) continue;
		c = *s++; if ((c < a) || (c > z)) continue;
		c = *s++; if ((c < a) || (c > z)) continue;
		c = *s++; if ((c < a) || (c > z)) continue;

		// We've now found 5 [a..z] characters in a row
		c = *s++;
		if ((c < a) || (c > z))  {
			*fivep = w;
			fivep += (__builtin_popcount(calc_key(w)) == 5);
		}

		// Just quickly find the next line
		while (c != nl)
			c = *s++;
	}

	// Bulk process all found unique 5 words
	// If no words to process, return now
	int num = fivep - fives;
	if (num == 0)
		return;

	// Bulk reserve where to place the words
	int pos = atomic_fetch_add(&num_words, num);
	fivep = fives;
	while (num--) {
		char *w = *fivep++;
		wordkeys[pos] = calc_key(w);

		// Copy word to word table as a single 64-bit copy
		*(uint64_t *)(words + (pos++ << 3)) = *(uint64_t *)w;
	}
} // find_words

void
file_reader(struct worker *work)
{
	uint32_t rn = work - workers;

	// The e = s + (READ_CHUNK + 1) below is done because each reader
	// (except the first) only starts at a newline.  If the reader
	// starts at the very start of a 5 letter word, that means that it
	// would skip that word.  By adding the extra 1 here, the reader
	// processing the chunk before it can catch the word that may
	// have been skipped by the reader ahead of it
	do {
		char *s = work->start;
		s += atomic_fetch_add(&file_pos, READ_CHUNK);
		char *e = s + (READ_CHUNK + 1);

		if (s > work->end)
			break;
		if (e > work->end)
			e = work->end;

		// Make sure to only start after a newline
		// if we are not at the start of the file
		if (s > work->start)
			while ((s < e) && (*s++ != '\n'));

		find_words(s, e, rn);
	} while (1);

	atomic_fetch_add(&readers_done, 1);
} // file_reader

uint64_t
process_words()
{
	uint64_t spins = 0;
	uint32_t cf[26] __attribute__ ((aligned(64))) = {0};

	// We do hash_init() and frq_init() here after the reader threads
	// start. This speeds up application load time as the OS needs to
	// clear less memory on startup.  Also, by initialising here, we
	// avoid blocking other work while initialisation occurs.

	// Build hash table and final key set
	hash_init();
	for (uint32_t *k = keys, key, pos = 0; ;) {
		if (pos >= num_words) {
			if (readers_done < num_readers) {
				spins++;
				asm("nop");
				continue;
			}
			if (pos >= num_words) {
				nkeys = k - keys;
				*k = 0;
				break;
			}
		}

		while ((key = wordkeys[pos]) == 0) {
			spins++;
			asm("nop");
		}

		if (hash_insert(key, pos++))
			*k++ = key;

		// Get character frequencies
		while (key) {
			cf[__builtin_ctz(key)]++;
			key &= key - 1;
		}
	}

	// All readers are done.  Collate character frequency stats
	frq_init();
	for (int c = 0; c < 26; c++)
		frq[c].f = cf[c];

	return spins;
} // process_words

void
start_solvers()
{
	go_solve = 1;
} // start_solvers


// We create a worker pool like this because on virtual systems, especially
// on WSL, thread-creation is very expensive, so we only want to do it once
void *
work_pool(void *arg)
{
	struct worker *work = (struct worker *)arg;
	int worker_num = work - workers;

	if (pthread_detach(pthread_self()))
		perror("pthread_detach");

	// Wait until told to start
	while (!workers_start)
		asm("nop");

	if (worker_num < num_readers)
		file_reader(work);

#ifndef NO_FREQ_SETUP
	while (1) {
		int set_num = atomic_fetch_add(&setup_set, 1);

		if (set_num >= 26)
			break;

		set_tier_offsets(frq + set_num);
	}
#endif

	// Not gonna lie.  This is ugly.  We're busy-waiting until we get
	// told to start solving.  It shouldn't be for too long though...
	// I tried many different methods but this was always the fastest
	while (!go_solve)
		asm("nop");

	solve_work();
	return NULL;
} // work_pool

void
spawn_readers(char *start, size_t len)
{
	char *end = start + len;

	num_readers = (len / READ_CHUNK) + 1;

	if (num_readers > MAX_READERS)
		num_readers = MAX_READERS;
	if (num_readers > nthreads)
		num_readers = nthreads;
	if (num_readers < 1)
		num_readers = 1;

	for (int i = 0; i < num_readers; i++) {
		workers[i].start = start;
		workers[i].end = end;
	}

	// Need to zero out the word table so that the main thread can
	// detect when a word key has been written by a reader thread
	if (num_readers > 1)
		memset(wordkeys, 0, sizeof(wordkeys));

	// Start any waiting workers
	workers_start = 1;

	// Check if main thread must do reading
	if (num_readers < 2)
		file_reader(workers);
	else
		atomic_fetch_add(&readers_done, 1);

	// The main thread processes the words as the reader threads find them
	process_words();
} // spawn_readers

// File Reader.  We use mmap() for efficiency for both reading and processing
void
read_words(char *path)
{
	int fd;


	if ((fd = open(path, O_RDONLY)) < 0) {
		perror("open");
		exit(EXIT_FAILURE);
	}

	struct stat statbuf[1];
	if (fstat(fd, statbuf) < 0) {
		perror("fstat");
		exit(EXIT_FAILURE);
	}

	size_t len = statbuf->st_size;
	char *addr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	// Safe to close file now.  mapping remains until munmap() is called
	close(fd);

	// Start file reader threads
	spawn_readers(addr, len);

	// We don't explicitly call munmap() as this can be slowish on some systems
	// Instead we'll just let the process terminate and it'll get unmapped then
} // read_words


// ********************* RESULTS WRITER ********************

// Solutions exists as a single character array assembled
// by the solver threads We just need to write it out.
void
emit_solutions()
{
	ssize_t len = num_sol * 30, written = 0;

	int solution_fd;
	if ((solution_fd = open(solution_filename, O_WRONLY | O_CREAT, 0644)) < 0) {
		fprintf(stderr, "Unable to open %s for writing\n", solution_filename);
		return;
	}

	// Truncate the file size if needed
	struct stat statbuf[1];
	if (fstat(solution_fd, statbuf) < 0) {
		perror("fstat");
		exit(EXIT_FAILURE);
	}

	if ((statbuf->st_size > len) && (ftruncate(solution_fd, len) < 0)) {
		perror("ftruncate");
		fprintf(stderr, "WARNING: Unable to truncate %s to %ld bytes\n",
			solution_filename, len);
	}

	// We loop here to handle any short writes that might occur
	while (written < len) {
		ssize_t ret = write(solution_fd, solutions + written, len - written);
		if (ret < 0) {
			fprintf(stderr, "Error writing to %s\n", solution_filename);
			perror("write");
			break;
		}
		written += ret;
	}

	close(solution_fd);
} // emit_solutions


// ********************* FREQUENCY HANDLER ********************

int
by_frequency_hi(const void *a, const void *b)
{
	return ((struct frequency *)b)->f - ((struct frequency *)a)->f;
} // by_frequency_hi


// Determine the values for set and end in a branchless manner
// The !! means we end up with only 0 or 1
#define CALCULATE_SET_AND_END						\
	do {								\
		struct tier *t = f->sets + !!(mask & f->tm1) +		\
					(!!(mask & f->tm2) << 1) +	\
					(!!(mask & f->tm3) << 2) +	\
					(!!(mask & f->tm4) << 3);	\
		int mf = !!(mask & f->tm5);				\
		int ms = !!(mask & f->tm6);				\
		end = t->s + (ms * t->toff3) + (!ms * t->l);		\
		ms &= !mf;						\
		set = t->s + ((mf & !ms) * t->toff2) + (ms * t->toff1);	\
	} while (0)

void
setup_tkeys(struct frequency *f)
{
	struct tier	*t0 = f->sets;
	uint32_t	tm1 = f->tm1, tm2 = f->tm2;
	uint32_t	tm3 = f->tm3, tm4 = f->tm4;
	uint32_t	*kp = t0->s + t0->l + NUM_POISON;
	uint32_t	*ks, len;
	uint32_t	masks[16];

	// XXX - Come up with a way to make this a loop
	masks[0]  = 0;
	masks[1]  = tm1;
	masks[2]  = tm2;
	masks[3]  = tm2 | tm1;
	masks[4]  = tm3;
	masks[5]  = tm3 | tm1;
	masks[6]  = tm3 | tm2;
	masks[7]  = tm3 | tm2 | tm1;

	masks[8]  = tm4;
	masks[9]  = tm4 | tm1;
	masks[10] = tm4 | tm2;
	masks[11] = tm4 | tm2 | tm1;
	masks[12] = tm4 | tm3;
	masks[13] = tm4 | tm3 | tm1;
	masks[14] = tm4 | tm3 | tm2;
	masks[15] = tm4 | tm3 | tm2 | tm1;

	for (uint32_t mask, i = 1; i < 16; i++) {
		struct tier *ts = f->sets + i;
		mask = masks[i];

		ks = t0->s;
		ts->s = kp;

		len = t0->toff1;
		while (len--)
			kp += !((*kp = *ks++) & mask);
		ts->toff1 = kp - ts->s;

		len = t0->toff2 - t0->toff1;
		while (len--)
			kp += !((*kp = *ks++) & mask);
		ts->toff2 = kp - ts->s;

		len = t0->toff3 - t0->toff2;
		while (len--)
			kp += !((*kp = *ks++) & mask);
		ts->toff3 = kp - ts->s;

		len = t0->l - t0->toff3;
		while (len--)
			kp += !((*kp = *ks++) & mask);
		ts->l = kp - ts->s;

		for (uint32_t p = NUM_POISON; p--; )
			*kp++ = (uint32_t)(~0);
	}
} // setup_tkeys

// This function looks like it's doing a lot, but because of good spatial
// and temportal localities each call typically takes ~1us on words_alpha
static void
set_tier_offsets(struct frequency *f)
{
	struct tier *t = f->sets;
	uint32_t key, mask, len;
	uint32_t *ks, *kp;

	// Wait here until all data is ready
	while (!f->ready_to_setup)
		asm("nop");

	// "poison" NUM_POISON ending values with all bits set
	ks = t->s + t->l;
	for (int p = NUM_POISON; p--; )
		*ks++ = (uint32_t)(~0);

	// Skip first set.  Nothing uses its subsets
	if (f == frq)
		goto set_tier_offsets_done;

	// "uaeios" are the best static defaults
	f->tm1 = 1 << ('u' - 'a');
	f->tm2 = 1 << ('a' - 'a');
	f->tm3 = 1 << ('e' - 'a');
	f->tm4 = 1 << ('i' - 'a');
	f->tm5 = 1 << ('o' - 'a');
	f->tm6 = 1 << ('s' - 'a');

	// Organise full set into 2 subsets, that which
	// has tm5 followed by that which does not

	mask = f->tm5;

	// First subset has tm5, and then not
	ks = kp = t->s;
	len = t->l;
	for (; len--; ++ks)
		if ((key = *ks) & mask) {
			*ks = *kp;
			*kp++ = key;
		}
	t->toff2 = kp - t->s;

	// Now organise the first tm5 subset into that which
	// has tm6 followed by that which does not, and then
	// the second tm5 subset into that which does not
	// have tm6 followed by that which does

	mask = f->tm6;

	// First tm1 subset has tm2 then not
	ks = kp = t->s;
	len = t->toff2;
	for (; len--; ++ks)
		if ((key = *ks) & mask) {
			*ks = *kp;
			*kp++ = key;
		}
	t->toff1 = kp - t->s;

	// Second tm5 subset does not have tm6 then has
	ks = kp = t->s + t->toff2;
	len = t->l - t->toff2;
	for (; len--; ++ks)
		if (!((key = *ks) & mask)) {
			*ks = *kp;
			*kp++ = key;
		}
	t->toff3 = kp - t->s;

	setup_tkeys(f);

set_tier_offsets_done:
	// Mark as done
	atomic_fetch_add(&setups_done, 1);
} // set_tier_offsets

// Specialised frequency sort, since we only need to swap the first 8 bytes
// of frequency sets at this point in time and each frequency set structure
// can be many hundreds of bytes, which wastes time if qsort is used
void
fsort()
{
	for (int i = 1; i < 26; ++i)
		for (int j = i; j; --j) {
			if (frq[j].f == 0)
				break;
			if (frq[j - 1].f && (frq[j].f > frq[j - 1].f))
				break;
			// Swap first 8 bytes
			uint64_t tmp = *(uint64_t *)(frq + j);
			*(uint64_t *)(frq + j) = *(uint64_t *)(frq + (j - 1));
			*(uint64_t *)(frq + (j - 1)) = tmp;
		}

	// Set the bit indices
	for (int i = 0, one = 1; i < 26; i++) {
		frq[i].b = __builtin_ctz(frq[i].m);
		unmap[frq[i].b] = (one << i);
	}
} // fsort

// The role of this function is to re-arrange the key set according to all
// words containing the least frequently used letter, and then scanning the
// remainder and so on until all keys have been assigned to sets. It achieves
// this by swapping keys in the key set and padding for any AVX operations
//
// Despite looking CPU and memory intensive, this function utilises strong
// spatial and temporal locality principles, and so runs in ~42us in practise
// without factoring in the calls to set_tier_offsets()
void
setup_frequency_sets()
{
	fsort();

	// Setup for key spray
	uint32_t *bp[32] __attribute__((aligned(64)));
	for (uint32_t i = 0; i < 26; i++)
		bp[i] = tkeys[i];

	// Spray keys to buckets
	for (uint32_t *kp = keys, key; (key = *kp++); ) {
		uint32_t mk = unmap[__builtin_ctz(key)];
		uint32_t k = key & (key - 1);

		mk |= unmap[__builtin_ctz(k)]; k &= k - 1;
		mk |= unmap[__builtin_ctz(k)]; k &= k - 1;
		mk |= unmap[__builtin_ctz(k)]; k &= k - 1;
		mk |= unmap[__builtin_ctz(k)];

		*bp[__builtin_ctz(mk)]++ = key;
	}

	// Determine min_search_depth and start worker threads
	for (int i = 0; i < 26; i++) {
		struct frequency *f = frq + i;
		struct tier *t = f->sets;

		t->s = tkeys[i];
		if ((t->l = bp[i] - t->s) > 0)
			min_search_depth = i - 3;

		// Instruct any waiting worker thread to start setup
		// but we have to do it ourselves if single threaded
		f->ready_to_setup = 1;
		if (nthreads == 1)
			set_tier_offsets(f);
	}

	// Wait for all setups to complete
	while(setups_done < 26)
		asm("nop");
} // setup_frequency_sets

#ifndef DONT_INCLUDE_MAIN

// ********************* MAIN SETUP AND OUTPUT ********************

int
main(int argc, char *argv[])
{
	struct timespec t1[1], t2[1], t3[1], t4[1], t5[1];
	char file[256];
	pthread_t tid[1];

	// Copy in the default file-name
	strcpy(file, "words_alpha.txt");

	nthreads = get_nthreads();

	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (!strncmp(argv[i], "-v", 2)) {
				write_metrics = 1;
				continue;
			}

			if (!strncmp(argv[i], "-f", 2)) {
				if ((i + 1) < argc) {
					strncpy(file, argv[i+1], 255);
					file[255] = '\0';
					i++;
					continue;
				}
			}

			if (!strncmp(argv[i], "-t", 2)) {
				if ((i + 1) < argc) {
					nthreads = atoi(argv[i+1]);
					i++;
					if (nthreads < 0)
						nthreads = 1;
					if (nthreads > MAX_THREADS)
						nthreads = MAX_THREADS;
					continue;
				}
			}

			printf("Usage: %s [-v] [-t num_threads] [-f filename]\n", argv[0]);
			exit(1);
		}
	}

	if (nthreads <= 0)
		nthreads = 1;
	if (nthreads > MAX_THREADS)
		nthreads = MAX_THREADS;

	for (int i = 1; i < nthreads; i++)
		pthread_create(tid, NULL, work_pool, workers + i);

	if (write_metrics) clock_gettime(CLOCK_MONOTONIC, t1);

	read_words(file);

	if (write_metrics) clock_gettime(CLOCK_MONOTONIC, t2);

	setup_frequency_sets();

	if (write_metrics) clock_gettime(CLOCK_MONOTONIC, t3);

	solve();

	if (write_metrics) clock_gettime(CLOCK_MONOTONIC, t4);

	emit_solutions();

	if (write_metrics) clock_gettime(CLOCK_MONOTONIC, t5);

	if (!write_metrics)
		exit(0);

	printf("\nFrequency Table:\n");
	for (int i = 0; i < 26; i++) {
		struct tier *t = frq[i].sets;
		char c = 'a' + __builtin_ctz(frq[i].m);
		printf("%c set_length=%4d  toff1=%4d, toff2=%4d, toff[3]=%4d\n",
			c, t->l, t->toff1, t->toff2, t->toff3);
	}
	printf("\n\n");

	printf("Num Unique Words  = %8d\n", nkeys);
	printf("Hash Collisions   = %8u\n", hash_collisions);
	printf("Number of threads = %8d\n", nthreads);

	printf("\nNUM SOLUTIONS = %d\n", num_sol);

	printf("\nTIMES TAKEN :\n");
	print_time_taken("Total", t1, t5);
	printf("\n");
	print_time_taken("File Load", t1, t2);
	print_time_taken("Frequency Set Build", t2, t3);
	print_time_taken("Main Algorithm", t3, t4);
	print_time_taken("Emit Results", t4, t5);

	exit(0);
} // main

#endif
