/* *****************************************************************************
Pub/Sub
***************************************************************************** */
#ifndef H_FACIL_IO_H /* development sugar, ignore */
#include "999 dev.h" /* development sugar, ignore */
#endif               /* development sugar, ignore */

/* *****************************************************************************
Letter / Message Object
***************************************************************************** */

enum {
  FIO_PUBSUB_FILTER_BIT = 1,
  FIO_PUBSUB_PROCESS_BIT = 2,
  FIO_PUBSUB_ROOT_BIT = 4,
  FIO_PUBSUB_SIBLINGS_BIT = 8,
  FIO_PUBSUB_CLUSTER_BIT = 16,
  FIO_PUBSUB_PATTERN_BIT = 64, /* TODO: do we need this one? */
  FIO_PUBSUB_JSON_BIT = 128,
} letter_info_bits_e;

typedef struct {
  fio_s *from;
  uint32_t channel_len;
  uint32_t message_len;
  char buf[];
} letter_s;

#define FIO_REF_NAME      letter
#define FIO_REF_FLEX_TYPE char
#include "fio-stl.h"

/*
Letter network format in bytes:
| 1 special info byte |
| 8 message UUID |
| 4 little endian message length (32 bit value) |
| 3 little endian channel length (32 bit value) |
| X bytes channel name + 1 NUL terminator|
| X bytes message length + 1 NUL terminator|

when a filter is used instead of a channel name, format is:
| 1 special info byte |
| 8 message UUID |
| 4 little endian message length |
| 4 little endian filter number (32 bit value) |
| X bytes message length + 1 NUL terminator|
*/

#define LETTER_HEADER_LENGTH 18 /* 16 + 2 NUL terminators */

/* allocates a new letter. */
FIO_IFUNC letter_s *letter_new(fio_s *from,
                               uint32_t channel_len,
                               uint32_t message_len,
                               char first_byte) {
  size_t is_filter =
      (((uint32_t)first_byte & FIO_PUBSUB_FILTER_BIT) / FIO_PUBSUB_FILTER_BIT);
  if ((!is_filter && (channel_len >> 24)) || (message_len >> 27))
    return NULL;
  size_t len =
      LETTER_HEADER_LENGTH + message_len + (channel_len * (is_filter ^ 1));
  letter_s *l = letter_new2(len);
  FIO_ASSERT_ALLOC(l);
  l->from = from;
  l->channel_len = channel_len & (0xFFFFFF | ((256 - is_filter) << 24));
  l->message_len = message_len;
  return l;
}

/** for filters place filter value in channel_len, channel will be ignored */
FIO_IFUNC letter_s *letter_author(fio_s *from,
                                  uint64_t message_id,
                                  char *channel,
                                  uint32_t channel_len,
                                  char *message,
                                  uint32_t message_len,
                                  uint8_t flags) {
  uint8_t is_filter = ((flags & FIO_PUBSUB_FILTER_BIT) / FIO_PUBSUB_FILTER_BIT);
  if ((!is_filter && (channel_len >> 24)) || (message_len >> 27))
    return NULL;
  size_t len =
      LETTER_HEADER_LENGTH + message_len + (channel_len * (is_filter ^ 1));
  letter_s *l = letter_new2(len);
  FIO_ASSERT_ALLOC(l);
  l->from = from;
  l->channel_len = channel_len;
  l->message_len = message_len;
  l->buf[0] = flags;
  fio_u2buf64_little(l->buf + 1, message_id);
  fio_u2buf32_little(l->buf + 9, message_len);
  fio_u2buf32_little(l->buf + 13, channel_len);
  channel_len *= (is_filter ^ 1);
  if (channel_len && channel) {
    memcpy(l->buf + 16, channel, channel_len);
    l->buf[16 + channel_len] = 0;
  }
  if (message_len && message) {
    memcpy(l->buf + 16 + 1 + channel_len, message, message_len);
  }
  l->buf[16 + 1 + channel_len + message_len] = 0;
  return l;
}

/* frees a letter's reference. */
#define letter_free letter_free2

/* returns 1 if a letter is bound to a filter, otherwise 0. */
FIO_IFUNC int32_t letter_is_filter(letter_s *l) {
  return (((uint32_t)l->buf[0] & FIO_PUBSUB_FILTER_BIT) /
          FIO_PUBSUB_FILTER_BIT);
}

/* returns a letter's ID (may be 0 for internal letters) */
FIO_IFUNC uint64_t letter_id(letter_s *l) {
  return fio_buf2u64_little(l->buf + 1);
}

/* returns a letter's channel (if none, returns the filter's address) */
FIO_IFUNC char *letter_channel(letter_s *l) {
  return l->buf + (16 * (letter_is_filter(l) ^ 1));
}

/* returns a letter's message length (if any) */
FIO_IFUNC size_t letter_message_len(letter_s *l) { return l->message_len; }

/* returns a letter's channel length (if any) */
FIO_IFUNC size_t letter_channel_len(letter_s *l) {
  return l->channel_len * (letter_is_filter(l) ^ 1);
}

/* returns a letter's filter (if any) */
FIO_IFUNC int32_t letter_filter(letter_s *l) {
  return (int32_t)l->channel_len * letter_is_filter(l);
}

/* returns a letter's message */
FIO_IFUNC char *letter_message(letter_s *l) {
  return l->buf + 1 + 16 + letter_channel_len(l);
}

/* returns a letter's length */
FIO_IFUNC size_t letter_len(letter_s *l) {
  return (size_t)2ULL + 16 + l->message_len + letter_channel_len(l);
}

/* write a letter to an IO object */
FIO_IFUNC void letter_write(fio_s *io, letter_s *l) {
  fio_write2(io,
             .buf = (char *)letter_dup2(l),
             .offset = (uintptr_t)l->buf - (uintptr_t)l,
             .len = letter_len(l),
             .dealloc = (void (*)(void *))letter_free2);
}

/* *****************************************************************************
Letter Reading, Parsing and Sending
***************************************************************************** */

typedef struct {
  letter_s *letter;
  size_t pos;
  char buf[LETTER_HEADER_LENGTH];
} letter_parser_s;

/* a new letter parser */
FIO_IFUNC letter_parser_s *letter_parser_new(void) {
  letter_parser_s *p = fio_malloc(sizeof(*p));
  FIO_ASSERT_ALLOC(p);
  p->letter = NULL;
  p->pos = 0;
  return p;
}

/* free a letter parser */
FIO_IFUNC void letter_parser_free(letter_parser_s *parser) {
  letter_free(parser->letter);
  fio_free(parser);
}

/* Returns -1 on terminal error, 0 on read operation. */
FIO_SFUNC int letter_read(fio_s *io,
                          letter_parser_s *parser,
                          void (*callback)(letter_s *letter)) {
  for (;;) {
    ssize_t r;
    if (parser->letter) {
      letter_s *const letter = parser->letter;
      const size_t to_read = letter_len(parser->letter);
      while (parser->pos < to_read) {
        r = fio_read(io, letter->buf + parser->pos, to_read - parser->pos);
        if (r <= 0)
          return 0;
        parser->pos += r;
      }
      callback(letter);
      letter_free(letter);
      parser->letter = NULL;
    }
    r = fio_read(io,
                 parser->buf + parser->pos,
                 LETTER_HEADER_LENGTH - parser->pos);
    if (r <= 0)
      return 0;
    parser->pos += r;
    if (parser->pos < 18)
      return 0;
    uint32_t channel_len = fio_buf2u32_little(parser->buf + 9);
    uint32_t message_len = fio_buf2u32_little(parser->buf + 13);
    parser->letter = letter_new(io, channel_len, message_len, parser->buf[0]);
    if (!parser->letter)
      return -1;
    memcpy(parser->letter->buf, parser->buf, parser->pos);
  }
}

/* *****************************************************************************
Pub/Sub - defaults and builtin pub/sub engines
***************************************************************************** */

/** Used to publish the message exclusively to the root / master process. */
const fio_pubsub_engine_s *const FIO_PUBSUB_ROOT =
    (fio_pubsub_engine_s *)FIO_PUBSUB_ROOT_BIT;
/** Used to publish the message only within the current process. */
const fio_pubsub_engine_s *const FIO_PUBSUB_PROCESS =
    (fio_pubsub_engine_s *)FIO_PUBSUB_PROCESS_BIT;
/** Used to publish the message except within the current process. */
const fio_pubsub_engine_s *const FIO_PUBSUB_SIBLINGS =
    (fio_pubsub_engine_s *)FIO_PUBSUB_SIBLINGS_BIT;
/** Used to publish the message for this process, its siblings and root. */
const fio_pubsub_engine_s *const FIO_PUBSUB_LOCAL =
    (fio_pubsub_engine_s *)(FIO_PUBSUB_SIBLINGS_BIT | FIO_PUBSUB_PROCESS_BIT |
                            FIO_PUBSUB_ROOT_BIT);
/** Used to publish the message to any possible publishers. */
const fio_pubsub_engine_s *const FIO_PUBSUB_CLUSTER =
    (fio_pubsub_engine_s *)(FIO_PUBSUB_CLUSTER_BIT | FIO_PUBSUB_SIBLINGS_BIT |
                            FIO_PUBSUB_PROCESS_BIT | FIO_PUBSUB_ROOT_BIT);

/** The default engine (settable). Initial default is FIO_PUBSUB_LOCAL. */
const fio_pubsub_engine_s *FIO_PUBSUB_DEFAULT = FIO_PUBSUB_CLUSTER;

/**
 * The pattern matching callback used for pattern matching.
 *
 * Returns 1 on a match or 0 if the string does not match the pattern.
 *
 * By default, the value is set to `fio_glob_match` (see facil.io's C STL).
 */
uint8_t (*FIO_PUBSUB_PATTERN_MATCH)(fio_str_info_s,
                                    fio_str_info_s) = fio_glob_match;

/* *****************************************************************************
Channel Objects
***************************************************************************** */

typedef enum {
  CHANNEL_TYPE_NAMED,
  CHANNEL_TYPE_FILTER,
  CHANNEL_TYPE_PATTERN,
  CHANNEL_TYPE_NONE,
} channel_type_e;

typedef struct {
  FIO_LIST_HEAD subscriptions;
  channel_type_e type;
  size_t name_len;
  char *name;
} channel_s;

FIO_IFUNC channel_s *channel_new(channel_type_e channel_type,
                                 char *name,
                                 size_t name_len) {
  channel_s *c = malloc(sizeof(*c) + name_len);
  FIO_ASSERT_ALLOC(c);
  *c = (channel_s){
      .subscriptions = FIO_LIST_INIT(c->subscriptions),
      .type = channel_type,
      .name_len = name_len,
      .name = (char *)(c + 1),
  };
  if (name_len)
    memcpy(c->name, name, name_len);
  return c;
}

FIO_IFUNC void channel_free(channel_s *channel) { free(channel); }

FIO_IFUNC _Bool channel_is_eq(channel_s *a, channel_s *b) {
  return a->name_len == b->name_len && !memcmp(a->name, b->name, a->name_len);
}

/* *****************************************************************************
Subscription Objects
***************************************************************************** */

typedef struct {
  FIO_LIST_NODE node;
  fio_s *io;
  channel_s *channel;
  void (*on_message)(fio_msg_s *msg);
  void (*on_unsubscribe)(void *udata);
  void *udata;
  uint32_t ref;
  fio_lock_i lock;
  uint8_t disabled; /* TODO: do we need this one? */
} subscription_s;

FIO_IFUNC subscription_s *subscription_new(fio_s *io,
                                           channel_s *channel,
                                           void (*on_message)(fio_msg_s *),
                                           void (*on_unsubscribe)(void *),
                                           void *udata) {
  subscription_s *s = (io ? fio_malloc : malloc)(sizeof(*s));
  FIO_ASSERT_ALLOC(s);
  *s = (subscription_s){
      .node = FIO_LIST_INIT(s->node),
      .io = io,
      .channel = channel,
      .on_message = on_message,
      .on_unsubscribe = on_unsubscribe,
      .udata = udata,
      .ref = 1,
  };
  return s;
}

/* we count subscription reference counts to make sure the udata is valid */
FIO_IFUNC subscription_s *subscription_dup(subscription_s *s) {
  FIO_ASSERT(fio_atomic_add_fetch(&s->ref, 1),
             "subscription reference count overflow detected!");
  return s;
}

FIO_SFUNC void subscription_on_unsubscribe___task(void *fnp, void *udata) {
  union {
    void *p;
    void (*fn)(void *udata);
  } u = {.p = fnp};
  u.fn(udata);
}

/* free the udata (and subscription) only after all callbacks return */
FIO_IFUNC void subscription_free(subscription_s *s) {
  if (fio_atomic_sub_fetch(&s->ref, 1))
    return;
  union {
    void *p;
    void (*fn)(void *udata);
  } u = {.fn = s->on_unsubscribe};
  fio_queue_push(FIO_QUEUE_USER,
                 subscription_on_unsubscribe___task,
                 u.p,
                 s->udata);
  (s->io ? fio_free : free)(s);
}

typedef struct {
  volatile size_t flag;
  letter_s *letter;
  fio_msg_s msg;
} fio_msg_internal_s;

FIO_IFUNC letter_s *fio_msg2letter(fio_msg_s *msg) {
  fio_msg_internal_s *mi = FIO_PTR_FROM_FIELD(fio_msg_internal_s, msg, msg);
  return mi->letter;
}

/** Defers the current callback, so it will be called again for the message. */
void fio_message_defer(fio_msg_s *msg) {
  fio_msg_internal_s *mi = FIO_PTR_FROM_FIELD(fio_msg_internal_s, msg, msg);
  mi->flag = 1;
}

FIO_SFUNC void subscription_deliver__task(void *s_, void *l_) {
  subscription_s *s = (subscription_s *)s_;
  letter_s *l = (letter_s *)l_;
  fio_msg_internal_s mi = {
      .flag = 0,
      .letter = l,
      .msg =
          {
              .io = s->io,
              .channel =
                  {
                      .buf = letter_channel(l),
                      .len = letter_channel_len(l),
                  },
              .message =
                  {
                      .buf = letter_message(l),
                      .len = letter_message_len(l),
                  },
              .udata = s->udata,
              .filter = letter_filter(l),
              .is_json =
                  ((l->buf[0] & FIO_PUBSUB_JSON_BIT) / FIO_PUBSUB_JSON_BIT),
          },
  };
  fio_lock_i *lock = (s->io ? &s->io->lock : &s->lock);
  if (fio_trylock(lock))
    goto reschedule;
  s->on_message(&mi.msg);
  fio_unlock(lock);
  if (mi.flag)
    goto reschedule;

  fio_undup(s->io);
  subscription_free(s);
  letter_free(l);
  return;
reschedule:
  fio_queue_push((s->io ? fio_queue_select(s->io->protocol->reserved.flags)
                        : FIO_QUEUE_USER),
                 subscription_deliver__task,
                 s,
                 l);
}

FIO_IFUNC void subscription_deliver(subscription_s *s, letter_s *l) {
  if (s->disabled)
    return;
  fio_queue_s *q = FIO_QUEUE_USER;
  if (s->io) {
    if (s->io == l->from || !fio_is_valid(s->io))
      return;
    q = fio_queue_select(s->io->protocol->reserved.flags);
    fio_dup(s->io);
  }
  fio_queue_push(q,
                 subscription_deliver__task,
                 subscription_dup(s),
                 letter_dup2(l));
}

/* *****************************************************************************
Postoffice
***************************************************************************** */

#define FIO_MAP_NAME                 channel_store
#define FIO_MAP_TYPE                 channel_s *
#define FIO_MAP_TYPE_CMP(a, b)       channel_is_eq(a, b)
#define FIO_MAP_TYPE_COPY(dest, src) ((dest) = (src))
#define FIO_MAP_TYPE_DESTROY(ch)     channel_free((ch))
#define FIO_MAP_TYPE_DISCARD(ch)     FIO_MAP_TYPE_DESTROY(ch)
#include "fio-stl.h"

/** The postoffice data store */
static struct {
  channel_store_s channels[CHANNEL_TYPE_NONE];
} postoffice = {
    .channels =
        {
            FIO_MAP_INIT,
            FIO_MAP_INIT,
            FIO_MAP_INIT,
        },
};

/* subscribe using a subscription object and a channel */
FIO_IFUNC void postoffice_subscribe(subscription_s *s) {
  if (!s)
    return;
  if (s->disabled || !s->channel || (s->io && !fio_is_valid(s->io)))
    goto error;
  const uint64_t hash =
      fio_risky_hash(s->channel->name, s->channel->name_len, 0);
  s->channel =
      channel_store_set_if_missing(&postoffice.channels[s->channel->type],
                                   hash,
                                   s->channel);
  FIO_LIST_PUSH(&s->channel->subscriptions, &s->node);
  return;

error:
  channel_free(s->channel);
  subscription_free(s);
}

/* unsubscribe using a subscription object */
FIO_IFUNC void postoffice_unsubscribe(subscription_s *s) {
  if (!s)
    return;
  channel_s *ch = s->channel;
  s->disabled = 1;
  FIO_LIST_REMOVE(&s->node);
  subscription_free(s);
  if (!ch || !FIO_LIST_IS_EMPTY(&ch->subscriptions))
    return;
  const uint64_t hash = fio_risky_hash(ch->name, ch->name_len, 0);
  channel_store_remove(&postoffice.channels[ch->type], hash, ch, NULL);
}

/* deliver a letter to all subscriptions in the relevant channels */
FIO_IFUNC void postoffice_deliver(letter_s *l) {
  channel_s ch_key = {
      .name_len = letter_channel_len(l) + (4 * letter_is_filter(l)),
      .name = letter_channel(l),
  };
  channel_store_s *store = postoffice.channels + letter_is_filter(l);
  const uint64_t hash = fio_risky_hash(ch_key.name, ch_key.name_len, 0);
  channel_s *ch = channel_store_get(store, hash, &ch_key);
  if (ch) {
    FIO_LIST_EACH(subscription_s, node, &ch->subscriptions, s) {
      subscription_deliver(s, l);
    }
  }
  if (letter_is_filter(l) ||
      !channel_store_count(&postoffice.channels[CHANNEL_TYPE_PATTERN]))
    return;

  fio_str_info_s name = {
      .buf = letter_channel(l),
      .len = letter_channel_len(l),
  };
  FIO_MAP_EACH(channel_store, &postoffice.channels[CHANNEL_TYPE_PATTERN], pos) {
    fio_str_info_s pat = {
        .buf = pos->obj->name,
        .len = pos->obj->name_len,
    };
    if (FIO_PUBSUB_PATTERN_MATCH(pat, name)) {
      FIO_LIST_EACH(subscription_s, node, &pos->obj->subscriptions, s) {
        subscription_deliver(s, l);
      }
    }
  }
}

/* *****************************************************************************
Pub/Sub - Subscribe / Unsubscribe
***************************************************************************** */

/* perform subscription in system thread */
FIO_SFUNC void fio_subscribe___task(void *ch_, void *s_) {
  channel_s *ch = ch_;
  subscription_s *s = s_;
  s->channel = ch;
  postoffice_subscribe(s);
}

void fio_subscribe___(void); /* sublimetext marker */
/**
 * Subscribes to either a filter OR a channel (never both).
 *
 * The on_unsubscribe callback will be called on failure.
 */
void fio_subscribe FIO_NOOP(subscribe_args_s args) {
  if (args.filter) {
    args.is_pattern = 0;
    args.channel.buf = (char *)&args.filter;
    args.channel.len = sizeof(args.filter);
  }
  channel_s *ch = channel_new((!!(args.is_pattern) * CHANNEL_TYPE_PATTERN) |
                                  ((!!args.filter) * CHANNEL_TYPE_FILTER),
                              args.channel.buf,
                              args.channel.len);
  subscription_s *s = subscription_new(args.io,
                                       NULL,
                                       args.on_message,
                                       args.on_unsubscribe,
                                       args.udata);
  fio_queue_push(FIO_QUEUE_SYSTEM, fio_subscribe___task, ch, s);
  fio_env_set(args.io,
              .type = (-1LL - args.is_pattern - (!!args.filter)),
              .name = args.channel,
              .on_close = ((void (*)(void *))postoffice_unsubscribe),
              .udata = s);
}

void fio_unsubscribe___(void); /* sublimetext marker */

/**
 * Cancels an existing subscriptions.
 *
 * Accepts the same arguments as `fio_subscribe`, except the `udata` and
 * callback details are ignored (no need to provide `udata` or callback
 * details).
 *
 * Returns -1 if the subscription could not be found. Otherwise returns 0.
 */
int fio_unsubscribe FIO_NOOP(subscribe_args_s args) {
  if (args.filter) {
    args.is_pattern = 0;
    args.channel.buf = (char *)&args.filter;
    args.channel.len = sizeof(args.filter);
  }
  return fio_env_remove(args.io,
                        .type = (-1LL - args.is_pattern - (!!args.filter)),
                        .name = args.channel);
}

/* *****************************************************************************
Pub/Sub - Publish
***************************************************************************** */
FIO_SFUNC void fio_publish___task(void *letter, void *ignr_) {
  postoffice_deliver(letter);
  letter_free(letter);
  (void)ignr_;
}
/**
 * Publishes a message to the relevant subscribers (if any).
 *
 * See `fio_publish_args_s` for details.
 *
 * By default the message is sent using the FIO_PUBSUB_CLUSTER engine (all
 * processes, including the calling process).
 *
 * To limit the message only to other processes (exclude the calling process),
 * use the FIO_PUBSUB_SIBLINGS engine.
 *
 * To limit the message only to the calling process, use the
 * FIO_PUBSUB_PROCESS engine.
 *
 * To publish messages to the pub/sub layer, the `.filter` argument MUST be
 * equal to 0 or missing.
 */
void fio_publish FIO_NOOP(fio_publish_args_s args) {
  if (!args.engine)
    args.engine = FIO_PUBSUB_DEFAULT;
  if ((uintptr_t)(args.engine) > 0XFF)
    goto external_engine;
  letter_s *l = letter_author(NULL,
                              (fio_data.is_master ? fio_rand64() : 0),
                              args.channel.buf,
                              args.channel.len,
                              args.message.buf,
                              args.message.len,
                              (uint8_t)(uintptr_t)args.engine |
                                  ((!!args.is_json) * FIO_PUBSUB_JSON_BIT));
  fio_queue_push(FIO_QUEUE_SYSTEM, fio_publish___task, l);
  return;
external_engine:
  args.engine->publish(args.engine, args.channel, args.message, args.is_json);
}

/* *****************************************************************************
 * Message metadata (advance usage API)
 **************************************************************************** */

/**
 * The number of different metadata callbacks that can be attached.
 *
 * Effects performance.
 *
 * The default value should be enough for the following metadata objects:
 * - WebSocket server headers.
 * - WebSocket client (header + masked message copy).
 * - EventSource (SSE) encoded named channel and message.
 */
#ifndef FIO_PUBSUB_METADATA_LIMIT
#define FIO_PUBSUB_METADATA_LIMIT 4
#endif

/**
 * Finds the message's metadata by it's ID. Returns the data or NULL.
 *
 * The ID is the value returned by fio_message_metadata_callback_set.
 *
 * Note: numeral channels don't have metadata attached.
 */
void *fio_message_metadata(fio_msg_s *msg, int id);

/**
 * It's possible to attach metadata to facil.io named messages (filter == 0)
 * before they are published.
 *
 * This allows, for example, messages to be encoded as network packets for
 * outgoing protocols (i.e., encoding for WebSocket transmissions), improving
 * performance in large network based broadcasting.
 *
 * Up to `FIO_PUBSUB_METADATA_LIMIT` metadata callbacks can be attached.
 *
 * The callback should return a `void *` pointer.
 *
 * To remove a callback, call `fio_message_metadata_remove` with the returned
 * value.
 *
 * The cluster messaging system allows some messages to be flagged as JSON and
 * this flag is available to the metadata callback.
 *
 * Returns a positive number on success (the metadata ID) or zero (0) on
 * failure.
 */
int fio_message_metadata_add(fio_msg_metadata_fn builder,
                             void (*cleanup)(void *));

/**
 * Removed the metadata callback.
 *
 * Removal might be delayed if live metatdata exists.
 */
void fio_message_metadata_remove(int id);

/* *****************************************************************************
 * Cluster / Pub/Sub Middleware and Extensions ("Engines")
 **************************************************************************** */

/**
 * Attaches an engine, so it's callback can be called by facil.io.
 *
 * The `subscribe` callback will be called for every existing channel.
 *
 * NOTE: the root (master) process will call `subscribe` for any channel in any
 * process, while all the other processes will call `subscribe` only for their
 * own channels. This allows engines to use the root (master) process as an
 * exclusive subscription process.
 */
void fio_pubsub_attach(fio_pubsub_engine_s *engine);

/** Detaches an engine, so it could be safely destroyed. */
void fio_pubsub_detach(fio_pubsub_engine_s *engine);

/**
 * Engines can ask facil.io to call the `subscribe` callback for all active
 * channels.
 *
 * This allows engines that lost their connection to their Pub/Sub service to
 * resubscribe all the currently active channels with the new connection.
 *
 * CAUTION: This is an evented task... try not to free the engine's memory while
 * resubscriptions are under way...
 *
 * NOTE: the root (master) process will call `subscribe` for any channel in any
 * process, while all the other processes will call `subscribe` only for their
 * own channels. This allows engines to use the root (master) process as an
 * exclusive subscription process.
 */
void fio_pubsub_reattach(fio_pubsub_engine_s *eng);

/** Returns true (1) if the engine is attached to the system. */
int fio_pubsub_is_attached(fio_pubsub_engine_s *engine);

/* *****************************************************************************
 * TODO: clusterfy the local network using UDP broadcasting for node discovery.
 **************************************************************************** */
#if 0
/**
 * Broadcasts to the local machine on `port` in order to auto-detect and connect
 * to peers, creating a cluster that shares all pub/sub messages.
 *
 * Retruns -1 on error (i.e., not called from the root/master process).
 *
 * Returns 0 on success.
 */
int fio_pubsub_clusterfy(const char *port, fio_tls_s *tls);
#endif

#ifdef TEST

FIO_SFUNC void FIO_NAME_TEST(io, letter)(void) {
  struct test_info {
    uint64_t id;
    char *channel;
    char *msg;
    int32_t filter;
  } test_info[] = {
      {
          42,
          "My Channel",
          "My channel Message",
          0,
      },
      {
          0,
          NULL,
          "My filter Message",
          1,
      },
      {0},
  };
  for (int i = 0;
       test_info[i].msg || test_info[i].channel || test_info[i].filter;
       ++i) {
    letter_s *l =
        letter_author((fio_s *)(test_info + i),
                      test_info[i].id,
                      test_info[i].channel,
                      (test_info[i].channel ? strlen(test_info[i].channel)
                                            : test_info[i].filter),
                      test_info[i].msg,
                      (test_info[i].msg ? strlen(test_info[i].msg) : 0),
                      test_info[i].filter ? FIO_PUBSUB_FILTER_BIT : 0);
    FIO_ASSERT(letter_id(l) == test_info[i].id,
               "message ID identity error, %llu != %llu",
               letter_id(l),
               test_info[i].id);
    FIO_ASSERT(letter_is_filter(l) == !!test_info[i].filter,
               "letter filter flag author error");
    if (letter_is_filter(l)) {
      FIO_ASSERT(letter_filter(l) == test_info[i].filter,
                 "filter identity error %d != %d",
                 letter_filter(l),
                 test_info[i].filter);
    }
    if (test_info[i].msg) {
      FIO_ASSERT(letter_message_len(l) == strlen(test_info[i].msg),
                 "letter message length error");
      FIO_ASSERT(
          !memcmp(letter_message(l), test_info[i].msg, letter_message_len(l)),
          "message identity error (%s != %.*s)",
          test_info[i].msg,
          (int)letter_message_len(l),
          letter_message(l));
    } else {
      FIO_ASSERT(!letter_message_len(l),
                 "letter message length error %d != 0",
                 letter_message_len(l));
    }
    if (test_info[i].channel) {
      FIO_ASSERT(letter_channel_len(l) == strlen(test_info[i].channel),
                 "letter channel length error");
      FIO_ASSERT(letter_channel(l) && !memcmp(letter_channel(l),
                                              test_info[i].channel,
                                              letter_channel_len(l)),
                 "channel identity error (%s != %.*s)",
                 test_info[i].channel,
                 (int)l->channel_len,
                 letter_channel(l));
    } else {
      FIO_ASSERT(!letter_channel_len(l), "letter channel length error");
    }
    letter_free(l);
  }
}
#endif /* TEST */