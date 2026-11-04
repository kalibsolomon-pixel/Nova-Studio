// SPDX-License-Identifier: GPL-3.0-or-later
#include "fabric_profile.h"

namespace nova::testing {

const QList<FabricProfile> &fabricProfiles() {
    // Sources verified 2026-11-02 (see docs/phase-3d1/PHASE_3D1_DEPENDENCIES.md):
    // FabricMC/fabric-example-mod branch 1.21.11 (loader 0.20.1, Loom 1.19,
    // plugin net.fabricmc.fabric-loom-remap, Gradle 9.8.2, Java 21);
    // services.gradle.org checksums; Modrinth API version metadata; Adoptium API.
    static const QList<FabricProfile> profiles = [] {
        FabricProfile p;
        p.key = "mc1.21.11-loader0.20.1-loom1.19.3-gradle9.8.2-rrls5.1.15";
        p.minecraftVersion = "1.21.11";
        p.gameJavaMajor = 21;
        p.gradleJavaMinimum = 25; // Loom 1.19.3 refuses older Gradle JVMs
        p.loaderVersion = "0.20.1";
        p.loomPluginId = "net.fabricmc.fabric-loom-remap";
        p.loomVersion = "1.19.3";
        p.gradleVersion = "9.8.2";
        p.gradleDistributionSha256 =
            "21bd5da2de49b642904077f38c0efdc7778e92b3ed0889392dca61b97e389607";
        // SHA-256 of sorted relative file names and SHA-256 digests from the verified ZIP.
        p.gradleDistributionTreeSha256 =
            "b0221918a969fee8c2884a506c8312cad67ad3050d8b421feaa8abe004af5bb6";
        p.gradleWrapperJarSha256 =
            "3b8a25775a69158b5ad2b1d17a80a88dc7b40a352a73eb27d06c612a7ce68e98";
        p.wrapperResource = ":/fabric-runtime/gradle-wrapper-9.8.2.jar";
        p.mods = {
            {"fabric-api", "Fabric API", "0.141.6+1.21.11", "fabric-api-0.141.6+1.21.11.jar",
             "https://cdn.modrinth.com/data/P7dR8mSH/versions/6qAuTtLR/"
             "fabric-api-0.141.6%2B1.21.11.jar",
             "sha512",
             "852d3682c4f353fd0ca86546af8578513e8bcf628377fa9d49c556bc59c5024c69ab56c92fc6dfba11fed"
             "19e4316a3f5636e7e07fd3d96f7043b531aef59b637",
             2426039, "Modrinth (FabricMC)", "Apache-2.0"},
            {"forgeconfigapiport", "Forge Config API Port", "21.11.1",
             "ForgeConfigAPIPort-v21.11.1-mc1.21.11-Fabric.jar",
             "https://cdn.modrinth.com/data/ohNO6lps/versions/uXrWPsCu/"
             "ForgeConfigAPIPort-v21.11.1-mc1.21.11-Fabric.jar",
             "sha512",
             "28791c992d613da14b8685505d3ef632ed53b5f1e1d517f0b41677d10f8419f192dfbde991308df6cda5d"
             "0f113c0aa8fc18ecf4a0834029403b16d2f68dc52d6",
             598009, "Modrinth (Fuzs)", "MPL-2.0"},
            {"rrls", "Remove Reloading Screen", "5.1.15+mc1.21.11-fabric",
             "rrlsFabric-5.1.15+mc.1.21.11.jar",
             "https://cdn.modrinth.com/data/ZP7xHXtw/versions/TCSbq6jC/"
             "rrlsFabric-5.1.15%2Bmc.1.21.11.jar",
             "sha512",
             "7f45e1ff141bd8a93bcebb1fe8ed612cdfc673253ff7952f1a0141ba01a8cae1c5d044a73b9f0b84a93b0"
             "db4b7f41a089ca8651900def7a1dcfac29cf63d135a",
             198379, "Modrinth (dimadencep)", "OSL-3.0"},
        };
        p.gameJdk = {
            "temurin-21",
            "Eclipse Temurin JDK",
            "21.0.13+9",
            "OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip",
            "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/"
            "OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip",
            "sha256",
            "a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9",
            205421281,
            "Eclipse Adoptium",
            "GPL-2.0-with-classpath-exception"};
        p.gradleJdk = {
            "temurin-25",
            "Eclipse Temurin JDK",
            "25.0.5+8",
            "OpenJDK25U-jdk_x64_windows_hotspot_25.0.5_8.zip",
            "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.5%2B8/"
            "OpenJDK25U-jdk_x64_windows_hotspot_25.0.5_8.zip",
            "sha256",
            "636bdc13dc649ddc7d5921c5c7d23cca6e44fd19d662b45cc9c02135a6c96c42",
            141285173,
            "Eclipse Adoptium",
            "GPL-2.0-with-classpath-exception"};
        return QList<FabricProfile>{p};
    }();
    return profiles;
}

const FabricProfile *fabricProfile(const QString &minecraftVersion) {
    for (const auto &profile : fabricProfiles())
        if (profile.minecraftVersion == minecraftVersion)
            return &profile;
    return nullptr;
}

QString defaultMinecraftVersion() { return "1.21.11"; }

} // namespace nova::testing
