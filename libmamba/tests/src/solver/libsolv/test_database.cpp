// Copyright (c) 2024, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <array>
#include <functional>

#include <doctest/doctest.h>

#include "mamba/core/util.hpp"
#include "mamba/solver/libsolv/database.hpp"
#include "mamba/specs/match_spec.hpp"
#include "mamba/specs/package_info.hpp"
#include "mamba/util/string.hpp"

#include "mambatests.hpp"

using namespace mamba;
using namespace mamba::solver;

namespace
{
    auto mkpkg(std::string name, std::string version, std::vector<std::string> deps = {})
        -> specs::PackageInfo
    {
        auto out = specs::PackageInfo();
        out.name = std::move(name);
        out.version = std::move(version);
        out.dependencies = std::move(deps);
        return out;
    }
}

TEST_SUITE("solver::libsolv::database")
{
    using PackageInfo = specs::PackageInfo;

    TEST_CASE("Create a database")
    {
        auto db = libsolv::Database({});
        CHECK(std::is_move_constructible_v<libsolv::Database>);
        CHECK_EQ(db.repo_count(), 0);

        SUBCASE("Add repo from packages")
        {
            auto pkgs = std::array{
                mkpkg("x", "1.0"),
                mkpkg("x", "2.0"),
                mkpkg("z", "1.0", { "x>=1.0" }),
            };
            auto repo1 = db.add_repo_from_packages(pkgs, "repo1");
            CHECK_EQ(db.repo_count(), 1);
            CHECK_EQ(db.package_count(), 3);
            CHECK_EQ(repo1.package_count(), 3);

            SUBCASE("Mark as installed repo")
            {
                CHECK_FALSE(db.installed_repo().has_value());
                db.set_installed_repo(repo1);
                CHECK_EQ(db.installed_repo().value(), repo1);

                SUBCASE("Remove repo")
                {
                    db.remove_repo(repo1);
                    CHECK_EQ(db.repo_count(), 0);
                    CHECK_FALSE(db.installed_repo().has_value());
                    CHECK_EQ(db.package_count(), 0);
                    CHECK_EQ(repo1.package_count(), 0);
                }
            }

            SUBCASE("Serialize repo")
            {
                auto tmp_dir = TemporaryDirectory();
                auto solv_file = tmp_dir.path() / "repo1.solv";

                auto origin = libsolv::RepodataOrigin{
                    /* .url= */ "https://repo.mamba.pm",
                    /* .etag= */ "etag",
                    /* .mod= */ "Fri, 11 Feb 2022 13:52:44 GMT",
                };
                auto repo1_copy = db.native_serialize_repo(repo1, solv_file, origin);
                CHECK_EQ(repo1_copy, repo1);

                SUBCASE("Read serialized repo")
                {
                    auto repo2 = db.add_repo_from_native_serialization(solv_file, origin, "conda-forge")
                                     .value();
                    CHECK_EQ(repo2.name(), origin.url);
                    CHECK_EQ(repo2.package_count(), repo1.package_count());
                    CHECK_NE(repo2, repo1);
                    CHECK_EQ(db.package_count(), repo1.package_count() + repo2.package_count());
                }

                SUBCASE("Fail reading outdated repo")
                {
                    for (auto attr : {
                             &libsolv::RepodataOrigin::url,
                             &libsolv::RepodataOrigin::etag,
                             &libsolv::RepodataOrigin::mod,
                         })
                    {
                        auto expected = origin;
                        std::invoke(attr, expected) = "";
                        auto maybe = db.add_repo_from_native_serialization(
                            solv_file,
                            expected,
                            "conda-forge"
                        );
                        CHECK_FALSE(maybe.has_value());
                    }
                }
            }

            SUBCASE("Iterate over packages")
            {
                auto repo2 = db.add_repo_from_packages(std::array{ mkpkg("z", "2.0") }, "repo1");

                SUBCASE("In a given repo")
                {
                    std::size_t count = 0;
                    db.for_each_package_in_repo(
                        repo2,
                        [&](const auto& p)
                        {
                            count++;
                            CHECK_EQ(p.name, "z");
                            CHECK_EQ(p.version, "2.0");
                        }
                    );
                    CHECK_EQ(count, 1);
                }

                SUBCASE("Matching a MatchSpec in multiple repos")
                {
                    std::size_t count = 0;
                    db.for_each_package_matching(
                        specs::MatchSpec::parse("z").value(),
                        [&](const auto& p)
                        {
                            count++;
                            CHECK_EQ(p.name, "z");
                        }
                    );
                    CHECK_EQ(count, 2);
                }

                SUBCASE("Matching a strict MatchSpec")
                {
                    std::size_t count = 0;
                    db.for_each_package_matching(
                        specs::MatchSpec::parse("z>1.0").value(),
                        [&](const auto& p)
                        {
                            count++;
                            CHECK_EQ(p.name, "z");
                        }
                    );
                    CHECK_EQ(count, 1);
                }

                SUBCASE("Depending on a given dependency")
                {
                    std::size_t count = 0;
                    db.for_each_package_depending_on(
                        specs::MatchSpec::parse("x").value(),
                        [&](const auto& p)
                        {
                            count++;
                            CHECK(util::any_starts_with(p.dependencies, "x"));
                        }
                    );
                    CHECK_EQ(count, 1);
                }
            }
        }

        SUBCASE("Add repo from repodata with no extra pip")
        {
            const auto repodata = mambatests::test_data_dir
                                  / "repodata/conda-forge-numpy-linux-64.json";
            auto repo1 = db.add_repo_from_repodata_json(
                repodata,
                "https://conda.anaconda.org/conda-forge/linux-64",
                "conda-forge",
                libsolv::PipAsPythonDependency::No
            );
            REQUIRE(repo1.has_value());

            CHECK_EQ(repo1->package_count(), 33);

            auto found_python = false;
            db.for_each_package_matching(
                specs::MatchSpec::parse("python").value(),
                [&](const specs::PackageInfo& pkg)
                {
                    found_python = true;
                    for (auto const& dep : pkg.dependencies)
                    {
                        CHECK_FALSE(util::contains(dep, "pip"));
                    }
                }
            );
            CHECK(found_python);
        }

        SUBCASE("Add repo from repodata with extra pip")
        {
            const auto repodata = mambatests::test_data_dir
                                  / "repodata/conda-forge-numpy-linux-64.json";
            auto repo1 = db.add_repo_from_repodata_json(
                repodata,
                "https://conda.anaconda.org/conda-forge/linux-64",
                "conda-forge",
                libsolv::PipAsPythonDependency::Yes
            );
            REQUIRE(repo1.has_value());

            CHECK_EQ(repo1->package_count(), 33);

            auto found_python = false;
            db.for_each_package_matching(
                specs::MatchSpec::parse("python").value(),
                [&](const specs::PackageInfo& pkg)
                {
                    found_python = true;
                    auto found_pip = false;
                    for (auto const& dep : pkg.dependencies)
                    {
                        found_pip |= util::contains(dep, "pip");
                    }
                    CHECK(found_pip);
                }
            );
            CHECK(found_python);
        }

        SUBCASE("Add repo from repodata only .tar.bz2")
        {
            const auto repodata = mambatests::test_data_dir
                                  / "repodata/conda-forge-numpy-linux-64.json";
            auto repo1 = db.add_repo_from_repodata_json(
                repodata,
                "https://conda.anaconda.org/conda-forge/linux-64",
                "conda-forge",
                libsolv::PipAsPythonDependency::No,
                libsolv::UseOnlyTarBz2::Yes
            );
            REQUIRE(repo1.has_value());
            CHECK_EQ(repo1->package_count(), 4);
        }
    }
}
