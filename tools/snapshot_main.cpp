/*
    Headless GUI snapshot tool.

    Renders the plugin editor offscreen at several window sizes and writes PNGs.
    Component::createComponentSnapshot paints into an offscreen Image, so this needs
    no display, no window server and no Screen Recording permission -- which is the
    only way to visually inspect the editor in a sandboxed/CI environment.

    Usage:  WarehouseSnapshot [output-directory]
*/

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

#include "PluginProcessor.h"

namespace
{
    struct WindowSize { juce::String name; int width; int height; };

    /* Sizes are DERIVED from the editor's own default bounds and its ComponentBoundsConstrainer
       rather than hardcoded. The default/minimum window size is a product decision that changes
       (760x520 -> 900x620 in the v0.2 GUI), and a hardcoded list silently goes stale: it would
       keep rendering at sizes the constrainer no longer allows, so the snapshots would stop
       showing what a user actually sees. Deriving them means this tool never needs editing again
       when the layout is resized. */
    std::vector<WindowSize> deriveSizes (juce::AudioProcessorEditor& editor)
    {
        const int defW = juce::jmax (1, editor.getWidth());
        const int defH = juce::jmax (1, editor.getHeight());

        int minW = defW, minH = defH, maxW = defW, maxH = defH;

        if (auto* c = editor.getConstrainer())
        {
            minW = juce::jmax (1, c->getMinimumWidth());
            minH = juce::jmax (1, c->getMinimumHeight());
            maxW = juce::jmax (minW, c->getMaximumWidth());
            maxH = juce::jmax (minH, c->getMaximumHeight());
        }

        // A constrainer left at its defaults reports an absurd maximum (INT_MAX-ish); cap the
        // "large" render so we do not try to allocate a gigapixel image.
        constexpr int renderCap = 3000;

        if (maxW > renderCap || maxH > renderCap)
        {
            const double scale = juce::jmin ((double) renderCap / (double) maxW,
                                             (double) renderCap / (double) maxH);
            maxW = juce::jmax (minW, (int) ((double) maxW * scale));
            maxH = juce::jmax (minH, (int) ((double) maxH * scale));
        }

        std::vector<WindowSize> out;
        out.push_back ({ "min",     minW, minH });
        out.push_back ({ "default", defW, defH });

        // Only render "large" if it is meaningfully bigger than the default, otherwise it is a
        // duplicate image with a misleading name.
        if (maxW > defW + 8 || maxH > defH + 8)
            out.push_back ({ "large", maxW, maxH });

        return out;
    }
}

namespace
{
    /** Finds a TextButton anywhere below `root` whose button text matches `label`.

        The page selector lives inside PageStrip and `setPage()` is private, so there is no API
        route to a non-default page. Rather than widen PageStrip's interface purely for a tool,
        this drives the GUI the way a user does: locate the page button and click it. That has the
        side benefit of exercising the real click path, so a page that fails to switch shows up
        here too.
    */
    juce::TextButton* findButtonByText (juce::Component& root, const juce::String& label)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
                if (button->getButtonText().equalsIgnoreCase (label))
                    return button;

            if (auto* found = findButtonByText (*child, label))
                return found;
        }

        return nullptr;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    // getChildFile resolves an absolute argument to itself, and a relative one against cwd.
    const auto outDir = argc > 1 ? cwd.getChildFile (juce::String (argv[1])) : cwd;
    outDir.createDirectory();

    ReverbAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    std::vector<WindowSize> sizes;

    {
        std::unique_ptr<juce::AudioProcessorEditor> probe (processor.createEditor());

        if (probe == nullptr)
        {
            std::printf ("[FAIL] createEditor() returned nullptr\n");
            return 1;
        }

        sizes = deriveSizes (*probe);

        std::printf ("editor reports default %d x %d; rendering %d size(s)\n\n",
                     probe->getWidth(), probe->getHeight(), (int) sizes.size());
    }

    int failures = 0;

    // ---- Every knob page, at the default size -------------------------------------------------
    //
    // Previously only the default (SPACE) page was ever rendered, so the other three shipped
    // having never been looked at. That is how a page ends up with a dead gap where a removed
    // control used to be, or a single lonely knob in a grid sized for four.
    {
        const char* pageLabels[] = { "SPACE", "TONE", "MOVE", "MIX" };

        for (const auto* label : pageLabels)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

            if (editor == nullptr)
            {
                std::printf ("[FAIL] page %-6s createEditor() returned nullptr\n", label);
                ++failures;
                continue;
            }

            editor->setSize (editor->getWidth(), editor->getHeight());

            auto* button = findButtonByText (*editor, label);

            if (button == nullptr)
            {
                std::printf ("[FAIL] page %-6s no page button with that label found\n", label);
                ++failures;
                continue;
            }

            // Invoke onClick directly rather than triggerClick(). triggerClick() dispatches
            // asynchronously, and a console app has no modal-loop pump to land it with
            // (MessageManager::runDispatchLoopUntil only exists under
            // JUCE_MODAL_LOOPS_PERMITTED), so every image would silently be the SPACE page --
            // a check that renders four identical PNGs and reports success. PageStrip wires
            // its tabs as plain onClick lambdas (PageStrip.cpp:216-219), so calling the lambda
            // is both synchronous and the exact same code path the click takes.
            if (! button->onClick)
            {
                std::printf ("[FAIL] page %-6s button has no onClick handler\n", label);
                ++failures;
                continue;
            }

            button->onClick();
            editor->resized();

            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);

            if (! image.isValid() || image.getWidth() <= 0)
            {
                std::printf ("[FAIL] page %-6s snapshot image invalid\n", label);
                ++failures;
                continue;
            }

            auto lower = juce::String (label).toLowerCase();
            const auto file = outDir.getChildFile ("warehouse-page-" + lower + ".png");
            file.deleteFile();

            if (auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
            {
                juce::PNGImageFormat png;

                if (png.writeImageToStream (image, *stream))
                    std::printf ("[ok]   page %-6s ->  %s\n", label, file.getFullPathName().toRawUTF8());
                else
                {
                    std::printf ("[FAIL] page %-6s PNG write failed\n", label);
                    ++failures;
                }
            }
            else
            {
                std::printf ("[FAIL] page %-6s could not open output stream\n", label);
                ++failures;
            }
        }

        std::printf ("\n");
    }

    for (const auto& size : sizes)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

        if (editor == nullptr)
        {
            std::printf ("[FAIL] %-8s createEditor() returned nullptr\n", size.name.toRawUTF8());
            ++failures;
            continue;
        }

        editor->setSize (size.width, size.height);

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);

        if (! image.isValid() || image.getWidth() <= 0)
        {
            std::printf ("[FAIL] %-8s snapshot image invalid\n", size.name.toRawUTF8());
            ++failures;
            continue;
        }

        const auto file = outDir.getChildFile (juce::String ("warehouse-gui-") + size.name + ".png");
        file.deleteFile();

        if (auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
        {
            juce::PNGImageFormat png;

            if (png.writeImageToStream (image, *stream))
                std::printf ("[ok]   %-8s %4d x %4d  ->  %s\n",
                             size.name.toRawUTF8(), image.getWidth(), image.getHeight(),
                             file.getFullPathName().toRawUTF8());
            else
            {
                std::printf ("[FAIL] %-8s PNG encode failed\n", size.name.toRawUTF8());
                ++failures;
            }
        }
        else
        {
            std::printf ("[FAIL] %-8s could not open %s\n", size.name.toRawUTF8(), file.getFullPathName().toRawUTF8());
            ++failures;
        }
    }

    std::printf ("%s (%d failures)\n", failures == 0 ? "SNAPSHOTS WRITTEN" : "SNAPSHOT FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
