// SPDX-License-Identifier: GPL-3.0-or-later
package studio.nova.companion;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.mojang.blaze3d.platform.NativeImage;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import java.util.regex.Pattern;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.gui.GuiGraphics;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.world.item.ItemStack;
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
    private static final long REQUEST_LIMIT = 16384;
    private static final int ICON_LIMIT = 64;

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
        final String type = text(request, "type");
        if ("icons".equals(type)) {
            icons(folder, launch, id, request);
            return;
        }
        if (!"reload".equals(type)) {
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

    // Renders inventory icons the way the game draws them in a slot: one screen of items on the
    // vanilla slot grey at the current GUI scale, captured from the game's own frame and cropped.
    private static void icons(Path folder, String launch, String id, JsonObject request) {
        final List<ItemStack> stacks = new ArrayList<>();
        final JsonArray missing = new JsonArray();
        final JsonElement items = request.get("items");
        if (items == null || !items.isJsonArray() || items.getAsJsonArray().isEmpty() ||
            items.getAsJsonArray().size() > ICON_LIMIT) {
            reply(folder, launch, id, "error", 0, "Ask for 1 to " + ICON_LIMIT + " items");
            return;
        }
        for (final JsonElement item : items.getAsJsonArray()) {
            final Identifier key = item.isJsonPrimitive() ? Identifier.tryParse(item.getAsString()) : null;
            final ItemStack stack = key != null && BuiltInRegistries.ITEM.containsKey(key)
                                        ? new ItemStack(BuiltInRegistries.ITEM.getValue(key))
                                        : ItemStack.EMPTY;
            if (stack.isEmpty())
                missing.add(item.isJsonPrimitive() ? item.getAsString() : "");
            stacks.add(stack);
        }
        final Path output = folder.resolve("icons-" + id);
        final Minecraft client = Minecraft.getInstance();
        client.execute(() -> {
            final long started = System.nanoTime();
            final Screen previous = client.screen;
            client.setScreen(new IconScreen(stacks, screen -> client.execute(() -> Screenshot.takeScreenshot(
                client.getMainRenderTarget(), image -> {
                    final JsonObject result = new JsonObject();
                    String error = null;
                    try (image) {
                        final int scale = client.getWindow().getGuiScale();
                        Files.createDirectories(output);
                        for (int i = 0; i < stacks.size(); ++i) {
                            final int size = IconScreen.SLOT * scale;
                            final int x = screen.cellX(i) * scale, y = screen.cellY(i) * scale;
                            if (x + size > image.getWidth() || y + size > image.getHeight()) {
                                error = "The game window is too small for " + stacks.size() + " icons";
                                break;
                            }
                            try (NativeImage icon = new NativeImage(size, size, false)) {
                                for (int row = 0; row < size; ++row)
                                    for (int column = 0; column < size; ++column)
                                        icon.setPixel(column, row, image.getPixel(x + column, y + row));
                                icon.writeToFile(output.resolve(i + ".png"));
                            }
                        }
                        result.addProperty("guiScale", scale);
                    } catch (IOException e) {
                        error = e.getMessage();
                    }
                    client.setScreen(previous);
                    result.addProperty("folder", output.getFileName().toString());
                    result.addProperty("background", String.format("#%06x", IconScreen.BACKGROUND & 0xffffff));
                    result.add("missing", missing);
                    respond(folder, launch, id, error == null ? "finished" : "failed",
                            (System.nanoTime() - started) / 1_000_000, error, result);
                }))));
        });
    }

    private static final class IconScreen extends Screen {
        static final int SLOT = 16, CELL = 24, MARGIN = 8, BACKGROUND = 0xFF8B8B8B;
        private final List<ItemStack> stacks;
        private final Consumer<IconScreen> captured;
        private int frames, columns = 1;

        IconScreen(List<ItemStack> stacks, Consumer<IconScreen> captured) {
            super(Component.literal("Nova Studio icons"));
            this.stacks = stacks;
            this.captured = captured;
        }

        int cellX(int index) { return MARGIN + (index % columns) * CELL; }
        int cellY(int index) { return MARGIN + (index / columns) * CELL; }

        @Override
        public void renderBackground(GuiGraphics graphics, int mouseX, int mouseY, float partialTick) {}

        @Override
        public void render(GuiGraphics graphics, int mouseX, int mouseY, float partialTick) {
            graphics.fill(0, 0, width, height, BACKGROUND);
            columns = Math.max(1, (width - 2 * MARGIN) / CELL);
            for (int i = 0; i < stacks.size(); ++i)
                if (!stacks.get(i).isEmpty())
                    graphics.renderFakeItem(stacks.get(i), cellX(i), cellY(i));
            // A few frames let textures and models settle; the capture runs between frames.
            if (++frames == 4)
                captured.accept(this);
        }

        @Override
        public boolean isPauseScreen() { return false; }
    }

    private static void reply(Path folder, String launch, String id, String status, long ms, String error) {
        respond(folder, launch, id, status, ms, error, new JsonObject());
    }

    private static void respond(Path folder, String launch, String id, String status, long ms, String error,
                                JsonObject response) {
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
