// SPDX-License-Identifier: GPL-3.0-or-later
package studio.nova.companion;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.regex.Pattern;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

// Nova Studio's companion in its managed test client. Nova writes request files into
// <game>/nova-companion; this mod answers them, doing the work on the client thread. It acts
// only for the launch id Nova passed as -Dnova.launch, so a request left from an earlier launch
// is ignored. There is no network listener: the folder belongs to Nova's own test instance.
public final class NovaCompanion implements ClientModInitializer {
    static final int PROTOCOL = 1;
    static final String FOLDER = "nova-companion";
    private static final Logger LOG = LoggerFactory.getLogger("Nova companion");
    private static final Pattern LAUNCH = Pattern.compile("[0-9a-f]{32}");
    private static final Pattern ID = Pattern.compile("[0-9a-f]{1,32}");
    private static final long REQUEST_LIMIT = 4096;

    @Override
    public void onInitializeClient() {
        final String launch = System.getProperty("nova.launch", "");
        if (!LAUNCH.matcher(launch).matches())
            return; // not started by Nova: stay inert
        final Path folder = FabricLoader.getInstance().getGameDir().resolve(FOLDER);
        try {
            Files.createDirectories(folder);
            final JsonObject hello = new JsonObject();
            hello.addProperty("protocol", PROTOCOL);
            hello.addProperty("launch", launch);
            write(folder, "companion.json", hello);
        } catch (IOException e) {
            LOG.warn("Nova companion disabled: {}", e.getMessage());
            return;
        }
        final Thread poller = new Thread(() -> poll(folder, launch), "Nova companion");
        poller.setDaemon(true);
        poller.start();
        LOG.info("Nova companion ready (protocol {})", PROTOCOL);
    }

    private static void poll(Path folder, String launch) {
        while (true) {
            try (DirectoryStream<Path> requests = Files.newDirectoryStream(folder, "request-*.json")) {
                for (final Path request : requests)
                    handle(folder, launch, request);
            } catch (IOException ignored) {
                // The folder can vanish while Nova resets it; try again on the next pass.
            }
            try {
                Thread.sleep(200);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    private static void handle(Path folder, String launch, Path file) {
        final JsonObject request;
        try {
            final boolean small = Files.size(file) <= REQUEST_LIMIT;
            final String text = small ? Files.readString(file, StandardCharsets.UTF_8) : "";
            Files.deleteIfExists(file); // consumed: Nova reads its absence as "started"
            if (!small)
                return;
            request = JsonParser.parseString(text).getAsJsonObject();
        } catch (Exception e) {
            return;
        }
        final String id = text(request, "id");
        if (!ID.matcher(id).matches() || !launch.equals(text(request, "launch")))
            return;
        if (!"reload".equals(text(request, "type"))) {
            reply(folder, launch, id, "error", 0, "Unsupported request type");
            return;
        }
        final Minecraft client = Minecraft.getInstance();
        client.execute(() -> {
            final long started = System.nanoTime();
            LOG.info("Nova requested a resource reload");
            client.reloadResourcePacks().whenComplete((ignored, error) -> {
                final long ms = (System.nanoTime() - started) / 1_000_000;
                reply(folder, launch, id, error == null ? "finished" : "failed", ms,
                      error == null ? null : String.valueOf(error.getMessage()));
            });
        });
    }

    private static void reply(Path folder, String launch, String id, String status, long ms, String error) {
        final JsonObject response = new JsonObject();
        response.addProperty("protocol", PROTOCOL);
        response.addProperty("launch", launch);
        response.addProperty("id", id);
        response.addProperty("status", status);
        response.addProperty("durationMs", ms);
        if (error != null)
            response.addProperty("error", error.length() > 300 ? error.substring(0, 300) : error);
        try {
            write(folder, "response-" + id + ".json", response);
        } catch (IOException e) {
            LOG.warn("Nova companion could not answer: {}", e.getMessage());
        }
    }

    private static String text(JsonObject object, String key) {
        return object.has(key) && object.get(key).isJsonPrimitive() ? object.get(key).getAsString() : "";
    }

    // Written beside the target, then renamed, so Nova never reads half a file.
    private static void write(Path folder, String name, JsonObject value) throws IOException {
        final Path partial = folder.resolve(name + ".partial");
        Files.writeString(partial, value.toString(), StandardCharsets.UTF_8);
        Files.move(partial, folder.resolve(name), StandardCopyOption.REPLACE_EXISTING,
                   StandardCopyOption.ATOMIC_MOVE);
    }
}
