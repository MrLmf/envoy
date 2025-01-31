#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "envoy/runtime/runtime.h"
#include "envoy/stats/scope.h"
#include "envoy/upstream/load_balancer.h"

#include "common/common/macros.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"
#include "common/upstream/upstream_impl.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Upstream {

class SubsetLoadBalancer : public LoadBalancer, Logger::Loggable<Logger::Id::upstream> {
public:
  SubsetLoadBalancer(
      LoadBalancerType lb_type, PrioritySet& priority_set, const PrioritySet* local_priority_set,
      ClusterStats& stats, Stats::Scope& scope, Runtime::Loader& runtime,
      Runtime::RandomGenerator& random, const LoadBalancerSubsetInfo& subsets,
      const absl::optional<envoy::api::v2::Cluster::RingHashLbConfig>& lb_ring_hash_config,
      const absl::optional<envoy::api::v2::Cluster::LeastRequestLbConfig>& least_request_config,
      const envoy::api::v2::Cluster::CommonLbConfig& common_config);
  ~SubsetLoadBalancer();

  // Upstream::LoadBalancer
  HostConstSharedPtr chooseHost(LoadBalancerContext* context) override;

private:
  using HostPredicate = std::function<bool(const Host&)>;

  void initSubsetSelectorMap();
  void initSelectorFallbackSubset(const envoy::api::v2::Cluster::LbSubsetConfig::LbSubsetSelector::
                                      LbSubsetSelectorFallbackPolicy&);
  HostConstSharedPtr chooseHostForSelectorFallbackPolicy(
      const envoy::api::v2::Cluster::LbSubsetConfig::LbSubsetSelector::
          LbSubsetSelectorFallbackPolicy& fallback_policy,
      LoadBalancerContext* context);

  // Represents a subset of an original HostSet.
  class HostSubsetImpl : public HostSetImpl {
  public:
    HostSubsetImpl(const HostSet& original_host_set, bool locality_weight_aware,
                   bool scale_locality_weight)
        : HostSetImpl(original_host_set.priority(), original_host_set.overprovisioningFactor()),
          original_host_set_(original_host_set), locality_weight_aware_(locality_weight_aware),
          scale_locality_weight_(scale_locality_weight) {}

    void update(const HostVector& hosts_added, const HostVector& hosts_removed,
                HostPredicate predicate);
    LocalityWeightsConstSharedPtr
    determineLocalityWeights(const HostsPerLocality& hosts_per_locality) const;

    void triggerCallbacks() { HostSetImpl::runUpdateCallbacks({}, {}); }
    bool empty() { return hosts().empty(); }

  private:
    const HostSet& original_host_set_;
    const bool locality_weight_aware_;
    const bool scale_locality_weight_;
  };

  // Represents a subset of an original PrioritySet.
  class PrioritySubsetImpl : public PrioritySetImpl {
  public:
    PrioritySubsetImpl(const SubsetLoadBalancer& subset_lb, HostPredicate predicate,
                       bool locality_weight_aware, bool scale_locality_weight);

    void update(uint32_t priority, const HostVector& hosts_added, const HostVector& hosts_removed);

    bool empty() { return empty_; }

    const HostSubsetImpl* getOrCreateHostSubset(uint32_t priority) {
      return reinterpret_cast<const HostSubsetImpl*>(&getOrCreateHostSet(priority));
    }

    void triggerCallbacks() {
      for (size_t i = 0; i < hostSetsPerPriority().size(); ++i) {
        runReferenceUpdateCallbacks(i, {}, {});
      }
    }

    void updateSubset(uint32_t priority, const HostVector& hosts_added,
                      const HostVector& hosts_removed, HostPredicate predicate) {
      reinterpret_cast<HostSubsetImpl*>(host_sets_[priority].get())
          ->update(hosts_added, hosts_removed, predicate);

      runUpdateCallbacks(hosts_added, hosts_removed);
    }

    // Thread aware LB if applicable.
    ThreadAwareLoadBalancerPtr thread_aware_lb_;
    // Current active LB.
    LoadBalancerPtr lb_;

  protected:
    HostSetImplPtr createHostSet(uint32_t priority,
                                 absl::optional<uint32_t> overprovisioning_factor) override;

  private:
    const PrioritySet& original_priority_set_;
    const HostPredicate predicate_;
    const bool locality_weight_aware_;
    const bool scale_locality_weight_;
    bool empty_ = true;
  };

  using HostSubsetImplPtr = std::shared_ptr<HostSubsetImpl>;
  using PrioritySubsetImplPtr = std::shared_ptr<PrioritySubsetImpl>;

  using SubsetMetadata = std::vector<std::pair<std::string, ProtobufWkt::Value>>;

  class LbSubsetEntry;
  struct SubsetSelectorMap;

  using LbSubsetEntryPtr = std::shared_ptr<LbSubsetEntry>;
  using SubsetSelectorMapPtr = std::shared_ptr<SubsetSelectorMap>;
  using ValueSubsetMap = std::unordered_map<HashedValue, LbSubsetEntryPtr>;
  using LbSubsetMap = std::unordered_map<std::string, ValueSubsetMap>;

  struct SubsetSelectorMap {
    std::unordered_map<std::string, SubsetSelectorMapPtr> subset_keys_;
    envoy::api::v2::Cluster::LbSubsetConfig::LbSubsetSelector::LbSubsetSelectorFallbackPolicy
        fallback_policy_;
  };

  // Entry in the subset hierarchy.
  class LbSubsetEntry {
  public:
    LbSubsetEntry() {}

    bool initialized() const { return priority_subset_ != nullptr; }
    bool active() const { return initialized() && !priority_subset_->empty(); }

    LbSubsetMap children_;

    // Only initialized if a match exists at this level.
    PrioritySubsetImplPtr priority_subset_;
  };

  // Create filtered default subset (if necessary) and other subsets based on current hosts.
  void refreshSubsets();
  void refreshSubsets(uint32_t priority);

  // Called by HostSet::MemberUpdateCb
  void update(uint32_t priority, const HostVector& hosts_added, const HostVector& hosts_removed);

  void updateFallbackSubset(uint32_t priority, const HostVector& hosts_added,
                            const HostVector& hosts_removed);
  void processSubsets(
      const HostVector& hosts_added, const HostVector& hosts_removed,
      std::function<void(LbSubsetEntryPtr)> update_cb,
      std::function<void(LbSubsetEntryPtr, HostPredicate, const SubsetMetadata&, bool)> cb);

  HostConstSharedPtr tryChooseHostFromContext(LoadBalancerContext* context, bool& host_chosen);

  absl::optional<
      envoy::api::v2::Cluster::LbSubsetConfig::LbSubsetSelector::LbSubsetSelectorFallbackPolicy>
  tryFindSelectorFallbackPolicy(LoadBalancerContext* context);

  bool hostMatches(const SubsetMetadata& kvs, const Host& host);

  LbSubsetEntryPtr
  findSubset(const std::vector<Router::MetadataMatchCriterionConstSharedPtr>& matches);

  LbSubsetEntryPtr findOrCreateSubset(LbSubsetMap& subsets, const SubsetMetadata& kvs,
                                      uint32_t idx);
  void forEachSubset(LbSubsetMap& subsets, std::function<void(LbSubsetEntryPtr)> cb);

  SubsetMetadata extractSubsetMetadata(const std::set<std::string>& subset_keys, const Host& host);
  std::string describeMetadata(const SubsetMetadata& kvs);

  const LoadBalancerType lb_type_;
  const absl::optional<envoy::api::v2::Cluster::RingHashLbConfig> lb_ring_hash_config_;
  const absl::optional<envoy::api::v2::Cluster::LeastRequestLbConfig> least_request_config_;
  const envoy::api::v2::Cluster::CommonLbConfig common_config_;
  ClusterStats& stats_;
  Stats::Scope& scope_;
  Runtime::Loader& runtime_;
  Runtime::RandomGenerator& random_;

  const envoy::api::v2::Cluster::LbSubsetConfig::LbSubsetFallbackPolicy fallback_policy_;
  const SubsetMetadata default_subset_metadata_;
  const std::vector<SubsetSelectorPtr> subset_selectors_;

  const PrioritySet& original_priority_set_;
  const PrioritySet* original_local_priority_set_;
  Common::CallbackHandle* original_priority_set_callback_handle_;

  LbSubsetEntryPtr fallback_subset_;
  LbSubsetEntryPtr panic_mode_subset_;

  LbSubsetEntryPtr selector_fallback_subset_any_;
  LbSubsetEntryPtr selector_fallback_subset_default_;

  // Forms a trie-like structure. Requires lexically sorted Host and Route metadata.
  LbSubsetMap subsets_;
  // Forms a trie-like structure of lexically sorted keys+fallback policy from subset
  // selectors configuration
  SubsetSelectorMapPtr selectors_;

  const bool locality_weight_aware_;
  const bool scale_locality_weight_;

  friend class SubsetLoadBalancerDescribeMetadataTester;
};

} // namespace Upstream
} // namespace Envoy
