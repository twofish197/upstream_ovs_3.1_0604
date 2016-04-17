/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2014, 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CLASSIFIER_H
#define CLASSIFIER_H 1

/* Flow classifier.
 *
 *
 * What?
 * =====
 *
 * A flow classifier holds any number of "rules", each of which specifies
 * values to match for some fields or subfields and a priority.  Each OpenFlow
 * table is implemented as a flow classifier.
 *
 * The classifier has two primary design goals.  The first is obvious: given a
 * set of packet headers, as quickly as possible find the highest-priority rule
 * that matches those headers.  The following section describes the second
 * goal.
 *
 *
 * "Un-wildcarding"
 * ================
 *
 * A primary goal of the flow classifier is to produce, as a side effect of a
 * packet lookup, a wildcard mask that indicates which bits of the packet
 * headers were essential to the classification result.  Ideally, a 1-bit in
 * any position of this mask means that, if the corresponding bit in the packet
 * header were flipped, then the classification result might change.  A 0-bit
 * means that changing the packet header bit would have no effect.  Thus, the
 * wildcarded bits are the ones that played no role in the classification
 * decision.
 *
 * Such a wildcard mask is useful with datapaths that support installing flows
 * that wildcard fields or subfields.  If an OpenFlow lookup for a TCP flow
 * does not actually look at the TCP source or destination ports, for example,
 * then the switch may install into the datapath a flow that wildcards the port
 * numbers, which in turn allows the datapath to handle packets that arrive for
 * other TCP source or destination ports without additional help from
 * ovs-vswitchd.  This is useful for the Open vSwitch software and,
 * potentially, for ASIC-based switches as well.
 *
 * Some properties of the wildcard mask:
 *
 *     - "False 1-bits" are acceptable, that is, setting a bit in the wildcard
 *       mask to 1 will never cause a packet to be forwarded the wrong way.
 *       As a corollary, a wildcard mask composed of all 1-bits will always
 *       yield correct (but often needlessly inefficient) behavior.
 *
 *     - "False 0-bits" can cause problems, so they must be avoided.  In the
 *       extreme case, a mask of all 0-bits is only correct if the classifier
 *       contains only a single flow that matches all packets.
 *
 *     - 0-bits are desirable because they allow the datapath to act more
 *       autonomously, relying less on ovs-vswitchd to process flow setups,
 *       thereby improving performance.
 *
 *     - We don't know a good way to generate wildcard masks with the maximum
 *       (correct) number of 0-bits.  We use various approximations, described
 *       in later sections.
 *
 *     - Wildcard masks for lookups in a given classifier yield a
 *       non-overlapping set of rules.  More specifically:
 *
 *       Consider an classifier C1 filled with an arbitrary collection of rules
 *       and an empty classifier C2.  Now take a set of packet headers H and
 *       look it up in C1, yielding a highest-priority matching rule R1 and
 *       wildcard mask M.  Form a new classifier rule R2 out of packet headers
 *       H and mask M, and add R2 to C2 with a fixed priority.  If one were to
 *       do this for every possible set of packet headers H, then this
 *       process would not attempt to add any overlapping rules to C2, that is,
 *       any packet lookup using the rules generated by this process matches at
 *       most one rule in C2.
 *
 * During the lookup process, the classifier starts out with a wildcard mask
 * that is all 0-bits, that is, fully wildcarded.  As lookup proceeds, each
 * step tends to add constraints to the wildcard mask, that is, change
 * wildcarded 0-bits into exact-match 1-bits.  We call this "un-wildcarding".
 * A lookup step that examines a particular field must un-wildcard that field.
 * In general, un-wildcarding is necessary for correctness but undesirable for
 * performance.
 *
 *
 * Basic Classifier Design
 * =======================
 *
 * Suppose that all the rules in a classifier had the same form.  For example,
 * suppose that they all matched on the source and destination Ethernet address
 * and wildcarded all the other fields.  Then the obvious way to implement a
 * classifier would be a hash table on the source and destination Ethernet
 * addresses.  If new classification rules came along with a different form,
 * you could add a second hash table that hashed on the fields matched in those
 * rules.  With two hash tables, you look up a given flow in each hash table.
 * If there are no matches, the classifier didn't contain a match; if you find
 * a match in one of them, that's the result; if you find a match in both of
 * them, then the result is the rule with the higher priority.
 *
 * This is how the classifier works.  In a "struct classifier", each form of
 * "struct cls_rule" present (based on its ->match.mask) goes into a separate
 * "struct cls_subtable".  A lookup does a hash lookup in every "struct
 * cls_subtable" in the classifier and tracks the highest-priority match that
 * it finds.  The subtables are kept in a descending priority order according
 * to the highest priority rule in each subtable, which allows lookup to skip
 * over subtables that can't possibly have a higher-priority match than already
 * found.  Eliminating lookups through priority ordering aids both classifier
 * primary design goals: skipping lookups saves time and avoids un-wildcarding
 * fields that those lookups would have examined.
 *
 * One detail: a classifier can contain multiple rules that are identical other
 * than their priority.  When this happens, only the highest priority rule out
 * of a group of otherwise identical rules is stored directly in the "struct
 * cls_subtable", with the other almost-identical rules chained off a linked
 * list inside that highest-priority rule.
 *
 * The following sub-sections describe various optimizations over this simple
 * approach.
 *
 *
 * Staged Lookup (Wildcard Optimization)
 * -------------------------------------
 *
 * Subtable lookup is performed in ranges defined for struct flow, starting
 * from metadata (registers, in_port, etc.), then L2 header, L3, and finally
 * L4 ports.  Whenever it is found that there are no matches in the current
 * subtable, the rest of the subtable can be skipped.
 *
 * Staged lookup does not reduce lookup time, and it may increase it, because
 * it changes a single hash table lookup into multiple hash table lookups.
 * It reduces un-wildcarding significantly in important use cases.
 *
 *
 * Prefix Tracking (Wildcard Optimization)
 * ---------------------------------------
 *
 * Classifier uses prefix trees ("tries") for tracking the used
 * address space, enabling skipping classifier tables containing
 * longer masks than necessary for the given address.  This reduces
 * un-wildcarding for datapath flows in parts of the address space
 * without host routes, but consulting extra data structures (the
 * tries) may slightly increase lookup time.
 *
 * Trie lookup is interwoven with staged lookup, so that a trie is
 * searched only when the configured trie field becomes relevant for
 * the lookup.  The trie lookup results are retained so that each trie
 * is checked at most once for each classifier lookup.
 *
 * This implementation tracks the number of rules at each address
 * prefix for the whole classifier.  More aggressive table skipping
 * would be possible by maintaining lists of tables that have prefixes
 * at the lengths encountered on tree traversal, or by maintaining
 * separate tries for subsets of rules separated by metadata fields.
 *
 * Prefix tracking is configured via OVSDB "Flow_Table" table,
 * "fieldspec" column.  "fieldspec" is a string map where a "prefix"
 * key tells which fields should be used for prefix tracking.  The
 * value of the "prefix" key is a comma separated list of field names.
 *
 * There is a maximum number of fields that can be enabled for any one
 * flow table.  Currently this limit is 3.
 *
 *
 * Partitioning (Lookup Time and Wildcard Optimization)
 * ----------------------------------------------------
 *
 * Suppose that a given classifier is being used to handle multiple stages in a
 * pipeline using "resubmit", with metadata (that is, the OpenFlow 1.1+ field
 * named "metadata") distinguishing between the different stages.  For example,
 * metadata value 1 might identify ingress rules, metadata value 2 might
 * identify ACLs, and metadata value 3 might identify egress rules.  Such a
 * classifier is essentially partitioned into multiple sub-classifiers on the
 * basis of the metadata value.
 *
 * The classifier has a special optimization to speed up matching in this
 * scenario:
 *
 *     - Each cls_subtable that matches on metadata gets a tag derived from the
 *       subtable's mask, so that it is likely that each subtable has a unique
 *       tag.  (Duplicate tags have a performance cost but do not affect
 *       correctness.)
 *
 *     - For each metadata value matched by any cls_rule, the classifier
 *       constructs a "struct cls_partition" indexed by the metadata value.
 *       The cls_partition has a 'tags' member whose value is the bitwise-OR of
 *       the tags of each cls_subtable that contains any rule that matches on
 *       the cls_partition's metadata value.  In other words, struct
 *       cls_partition associates metadata values with subtables that need to
 *       be checked with flows with that specific metadata value.
 *
 * Thus, a flow lookup can start by looking up the partition associated with
 * the flow's metadata, and then skip over any cls_subtable whose 'tag' does
 * not intersect the partition's 'tags'.  (The flow must also be looked up in
 * any cls_subtable that doesn't match on metadata.  We handle that by giving
 * any such cls_subtable TAG_ALL as its 'tags' so that it matches any tag.)
 *
 * Partitioning saves lookup time by reducing the number of subtable lookups.
 * Each eliminated subtable lookup also reduces the amount of un-wildcarding.
 *
 *
 * Classifier Versioning
 * =====================
 *
 * Classifier lookups are always done in a specific classifier version, where
 * a version is defined to be a natural number.
 *
 * When a new rule is added to a classifier, it is set to become visible in a
 * specific version.  If the version number used at insert time is larger than
 * any version number currently used in lookups, the new rule is said to be
 * invisible to lookups.  This means that lookups won't find the rule, but the
 * rule is immediately available to classifier iterations.
 *
 * Similarly, a rule can be marked as to be deleted in a future version.  To
 * delete a rule in a way to not remove the rule before all ongoing lookups are
 * finished, the rule should be made invisible in a specific version number.
 * Then, when all the lookups use a later version number, the rule can be
 * actually removed from the classifier.
 *
 * Classifiers can hold duplicate rules (rules with the same match criteria and
 * priority) when at most one of these duplicates is visible in any given
 * lookup version.  The caller responsible for classifier modifications must
 * maintain this invariant.
 *
 * The classifier supports versioning for two reasons:
 *
 *     1. Support for versioned modifications makes it possible to perform an
 *        arbitraty series of classifier changes as one atomic transaction,
 *        where intermediate versions of the classifier are not visible to any
 *        lookups.  Also, when a rule is added for a future version, or marked
 *        for removal after the current version, such modifications can be
 *        reverted without any visible effects to any of the current lookups.
 *
 *     2. Performance: Adding (or deleting) a large set of rules can, in
 *        pathological cases, have a cost proportional to the number of rules
 *        already in the classifier.  When multiple rules are being added (or
 *        deleted) in one go, though, this pathological case cost can be
 *        typically avoided, as long as it is OK for any new rules to be
 *        invisible until the batch change is complete.
 *
 * Note that the classifier_replace() function replaces a rule immediately, and
 * is therefore not safe to use with versioning.  It is still available for the
 * users that do not use versioning.
 *
 *
 * Deferred Publication
 * ====================
 *
 * Removing large number of rules from classifier can be costly, as the
 * supporting data structures are teared down, in many cases just to be
 * re-instantiated right after.  In the worst case, as when each rule has a
 * different match pattern (mask), the maintenance of the match patterns can
 * have cost O(N^2), where N is the number of different match patterns.  To
 * alleviate this, the classifier supports a "deferred mode", in which changes
 * in internal data structures needed for future version lookups may not be
 * fully computed yet.  The computation is finalized when the deferred mode is
 * turned off.
 *
 * This feature can be used with versioning such that all changes to future
 * versions are made in the deferred mode.  Then, right before making the new
 * version visible to lookups, the deferred mode is turned off so that all the
 * data structures are ready for lookups with the new version number.
 *
 * To use deferred publication, first call classifier_defer().  Then, modify
 * the classifier via additions (classifier_insert() with a specific, future
 * version number) and deletions (use cls_rule_make_removable_after_version()).
 * Then call classifier_publish(), and after that, announce the new version
 * number to be used in lookups.
 *
 *
 * Thread-safety
 * =============
 *
 * The classifier may safely be accessed by many reader threads concurrently
 * and by a single writer, or by multiple writers when they guarantee mutually
 * exlucive access to classifier modifications.
 *
 * Since the classifier rules are RCU protected, the rule destruction after
 * removal from the classifier must be RCU postponed.  Also, when versioning is
 * used, the rule removal itself needs to be typically RCU postponed.  In this
 * case the rule destruction is doubly RCU postponed, i.e., the second
 * ovsrcu_postpone() call to destruct the rule is called from the first RCU
 * callback that removes the rule.
 *
 * Rules that have never been visible to lookups are an exeption to the above
 * rule.  Such rules can be removed immediately, but their destruction must
 * still be RCU postponed, as the rule's visibility attribute may be examined
 * parallel to the rule's removal. */

#include "cmap.h"
#include "openvswitch/match.h"
#include "openvswitch/meta-flow.h"
#include "pvector.h"
#include "rculist.h"
#include "openvswitch/type-props.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Classifier internal data structures. */
struct cls_subtable;
struct cls_match;

struct trie_node;
typedef OVSRCU_TYPE(struct trie_node *) rcu_trie_ptr;

/* Prefix trie for a 'field' */
struct cls_trie {
    const struct mf_field *field; /* Trie field, or NULL. */
    rcu_trie_ptr root;            /* NULL if none. */
};

typedef uint64_t cls_version_t;

#define CLS_MIN_VERSION 0                  /* Default version number to use. */
#define CLS_MAX_VERSION (TYPE_MAXIMUM(cls_version_t) - 1)
#define CLS_NOT_REMOVED_VERSION TYPE_MAXIMUM(cls_version_t)

enum {
    CLS_MAX_INDICES = 3,   /* Maximum number of lookup indices per subtable. */
    CLS_MAX_TRIES = 3      /* Maximum number of prefix trees per classifier. */
};

/* A flow classifier. */
struct classifier {
    int n_rules;                    /* Total number of rules. */
    uint8_t n_flow_segments;
    uint8_t flow_segments[CLS_MAX_INDICES]; /* Flow segment boundaries to use
                                             * for staged lookup. */
    struct cmap subtables_map;      /* Contains "struct cls_subtable"s.  */
    struct pvector subtables;
    struct cmap partitions;         /* Contains "struct cls_partition"s. */
    struct cls_trie tries[CLS_MAX_TRIES]; /* Prefix tries. */
    unsigned int n_tries;
    bool publish;                   /* Make changes visible to lookups? */
};

struct cls_conjunction {
    uint32_t id;
    uint8_t clause;
    uint8_t n_clauses;
};

/* A rule to be inserted to the classifier. */
struct cls_rule {
    struct rculist node;          /* In struct cls_subtable 'rules_list'. */
    const int priority;           /* Larger numbers are higher priorities. */
    OVSRCU_TYPE(struct cls_match *) cls_match;  /* NULL if not in a
                                                 * classifier. */
    const struct minimatch match; /* Matching rule. */
};

void cls_rule_init(struct cls_rule *, const struct match *, int priority);
void cls_rule_init_from_minimatch(struct cls_rule *, const struct minimatch *,
                                  int priority);
void cls_rule_clone(struct cls_rule *, const struct cls_rule *);
void cls_rule_move(struct cls_rule *dst, struct cls_rule *src);
void cls_rule_destroy(struct cls_rule *);

void cls_rule_set_conjunctions(struct cls_rule *,
                               const struct cls_conjunction *, size_t n);

bool cls_rule_equal(const struct cls_rule *, const struct cls_rule *);
void cls_rule_format(const struct cls_rule *, struct ds *);
bool cls_rule_is_catchall(const struct cls_rule *);
bool cls_rule_is_loose_match(const struct cls_rule *rule,
                             const struct minimatch *criteria);
bool cls_rule_visible_in_version(const struct cls_rule *, cls_version_t);
void cls_rule_make_invisible_in_version(const struct cls_rule *,
                                        cls_version_t);
void cls_rule_restore_visibility(const struct cls_rule *);

/* Constructor/destructor.  Must run single-threaded. */
void classifier_init(struct classifier *, const uint8_t *flow_segments);
void classifier_destroy(struct classifier *);

/* Modifiers.  Caller MUST exclude concurrent calls from other threads. */
bool classifier_set_prefix_fields(struct classifier *,
                                  const enum mf_field_id *trie_fields,
                                  unsigned int n_trie_fields);
void classifier_insert(struct classifier *, const struct cls_rule *,
                       cls_version_t, const struct cls_conjunction *,
                       size_t n_conjunctions);
const struct cls_rule *classifier_replace(struct classifier *,
                                          const struct cls_rule *,
                                          cls_version_t,
                                          const struct cls_conjunction *,
                                          size_t n_conjunctions);
const struct cls_rule *classifier_remove(struct classifier *,
                                         const struct cls_rule *);
static inline void classifier_defer(struct classifier *);
static inline void classifier_publish(struct classifier *);

/* Lookups.  These are RCU protected and may run concurrently with modifiers
 * and each other. */
const struct cls_rule *classifier_lookup(const struct classifier *,
                                         cls_version_t, struct flow *,
                                         struct flow_wildcards *);
bool classifier_rule_overlaps(const struct classifier *,
                              const struct cls_rule *, cls_version_t);
const struct cls_rule *classifier_find_rule_exactly(const struct classifier *,
                                                    const struct cls_rule *,
                                                    cls_version_t);
const struct cls_rule *classifier_find_match_exactly(const struct classifier *,
                                                     const struct match *,
                                                     int priority,
                                                     cls_version_t);
bool classifier_is_empty(const struct classifier *);
int classifier_count(const struct classifier *);

/* Iteration.
 *
 * Iteration is lockless and RCU-protected.  Concurrent threads may perform all
 * kinds of concurrent modifications without ruining the iteration.  Obviously,
 * any modifications may or may not be visible to the concurrent iterator, but
 * all the rules not deleted are visited by the iteration.  The iterating
 * thread may also modify the classifier rules itself.
 *
 * 'TARGET' iteration only iterates rules matching the 'TARGET' criteria.
 * Rather than looping through all the rules and skipping ones that can't
 * match, 'TARGET' iteration skips whole subtables, if the 'TARGET' happens to
 * be more specific than the subtable. */
struct cls_cursor {
    const struct classifier *cls;
    const struct cls_subtable *subtable;
    const struct cls_rule *target;
    cls_version_t version;   /* Version to iterate. */
    struct pvector_cursor subtables;
    const struct cls_rule *rule;
};

struct cls_cursor cls_cursor_start(const struct classifier *,
                                   const struct cls_rule *target,
                                   cls_version_t);
void cls_cursor_advance(struct cls_cursor *);

#define CLS_FOR_EACH(RULE, MEMBER, CLS)             \
    CLS_FOR_EACH_TARGET(RULE, MEMBER, CLS, NULL, CLS_MAX_VERSION)
#define CLS_FOR_EACH_TARGET(RULE, MEMBER, CLS, TARGET, VERSION)         \
    for (struct cls_cursor cursor__ = cls_cursor_start(CLS, TARGET, VERSION); \
         (cursor__.rule                                                 \
          ? (INIT_CONTAINER(RULE, cursor__.rule, MEMBER),               \
             cls_cursor_advance(&cursor__),                             \
             true)                                                      \
          : false);                                                     \
        )


static inline void
classifier_defer(struct classifier *cls)
{
    cls->publish = false;
}

static inline void
classifier_publish(struct classifier *cls)
{
    cls->publish = true;
    pvector_publish(&cls->subtables);
}

#ifdef __cplusplus
}
#endif
#endif /* classifier.h */
