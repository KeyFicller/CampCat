#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace campcat {

/// UI browsing only. Execution continues to use its own config snapshot.
struct workspace_selection {
    std::filesystem::path home;
    std::string graph;
    std::string bundle;

    void set_home(std::filesystem::path _home) {
        if (home != _home) {
            graph.clear();
            bundle.clear();
        }
        home = std::move(_home);
    }

    void select_graph(std::string _graph) {
        if (graph != _graph) {
            graph = std::move(_graph);
            bundle.clear();
        }
    }

    void select_bundle(std::string _graph, std::string _bundle) {
        graph = std::move(_graph);
        bundle = std::move(_bundle);
    }

    void reconcile(const std::vector<std::string> &_graphs,
                   const std::vector<std::string> &_bundles) {
        if (!graph.empty() &&
            std::find(_graphs.begin(), _graphs.end(), graph) == _graphs.end()) {
            select_graph({});
        }
        if (!bundle.empty() &&
            std::find(_bundles.begin(), _bundles.end(), bundle) == _bundles.end()) {
            bundle.clear();
        }
    }
};

} // namespace campcat
