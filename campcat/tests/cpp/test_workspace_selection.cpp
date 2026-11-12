#include "app/workspace_selection.h"

#include <gtest/gtest.h>

namespace {

TEST(WorkspaceSelection, RevisitingTheSameGraphKeepsTheSelectedSkill) {
    campcat::workspace_selection selection;
    selection.select_bundle("g", "g.city.recruit");
    selection.select_graph("g");
    EXPECT_EQ(selection.bundle, "g.city.recruit");
}

TEST(WorkspaceSelection, ChangingGraphsDropsThePreviousSkill) {
    campcat::workspace_selection selection;
    selection.select_bundle("g", "g.city");
    selection.select_graph("other");
    EXPECT_EQ(selection.graph, "other");
    EXPECT_TRUE(selection.bundle.empty());
}

TEST(WorkspaceSelection, ASelectedSkillCarriesItsGraphAcrossViews) {
    campcat::workspace_selection selection;
    selection.select_graph("old");
    selection.select_bundle("g", "g.city-recruit");
    selection.reconcile({"old", "g"}, {"g.city-recruit"});
    EXPECT_EQ(selection.graph, "g");
    EXPECT_EQ(selection.bundle, "g.city-recruit");
}

TEST(WorkspaceSelection, DeletedSkillsClearWithoutLosingTheGraph) {
    campcat::workspace_selection selection;
    selection.select_bundle("g", "g.city");
    selection.reconcile({"g"}, {});
    EXPECT_EQ(selection.graph, "g");
    EXPECT_TRUE(selection.bundle.empty());
}

TEST(WorkspaceSelection, DeletedGraphsDoNotLeaveAnUnrelatedSkillSelected) {
    campcat::workspace_selection selection;
    selection.select_bundle("gone", "gone.city");
    selection.reconcile({"other"}, {"gone.city"});
    EXPECT_TRUE(selection.graph.empty());
    EXPECT_TRUE(selection.bundle.empty());
}

TEST(WorkspaceSelection, AStandaloneSkillDoesNotNeedAGraph) {
    campcat::workspace_selection selection;
    selection.select_bundle("", "standalone");
    selection.reconcile({}, {"standalone"});
    EXPECT_TRUE(selection.graph.empty());
    EXPECT_EQ(selection.bundle, "standalone");
}

TEST(WorkspaceSelection, ChangingLibrariesClearsNamesFromTheOldLibrary) {
    campcat::workspace_selection selection;
    selection.set_home("/library/one");
    selection.select_bundle("g", "g.city");
    selection.set_home("/library/one");
    EXPECT_EQ(selection.bundle, "g.city");
    selection.set_home("/library/two");
    EXPECT_TRUE(selection.graph.empty());
    EXPECT_TRUE(selection.bundle.empty());
}

} // namespace
