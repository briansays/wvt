# Writing the documentation

## Files

- The pages are Markdown files in `docs/`. Sphinx reads them with the MyST parser.
- `docs/index.md` has the table of contents. When you add a page, add it to a `toctree` there.
- `docs/design/` has the design records of each feature. Refer to
  [Design records](design/index.md).
- `README.md` stays short. It has a description, a quick start and links to these pages.

## Build the pages

```sh
pip install -r docs/requirements.txt
sphinx-build -W -b html docs build/docs
```

Open `build/docs/index.html`. Before you push, make sure that the build has no warnings. CI uses
the same command.

## Links

- To link to a page, use its file name: `[Audio](audio.md)`.
- To link to a section, add the heading anchor: `[Recording](live.md#recording)`. The anchor is the
  heading in lower case, with hyphens in place of spaces.
- GitHub also shows these links correctly.

## Writing rules

The pages use ASD-STE100 Simplified Technical English. These are the most important rules:

- **Short sentences.** A procedural sentence has a maximum of 20 words. A descriptive sentence has
  a maximum of 25 words.
- **One topic in a paragraph.** A paragraph has a maximum of six sentences.
- **One instruction in a sentence.** Write the instructions as numbered steps, in the order that
  the user does them.
- **The imperative for instructions:** "Click **Start**", not "You should click **Start**".
- **The active voice:** "wvt writes the file", not "The file is written".
- **Simple tenses:** use the present tense for descriptions. Do not use "would" or "could".
- **Approved words, with one meaning.** Use "start" and "stop", not "launch" and "kill". Do not use
  phrasal verbs such as "set up" or "turn on" where a single verb is available.
- **Articles.** Use "a", "an" and "the" before nouns.
- **Warnings first.** Put a warning before the step that it applies to. Use a `{warning}` block.
- **Technical names** are permitted: menu items, buttons, options, file names and units. Write UI
  labels in bold, as they show on the screen.

For the full specification, refer to the ASD-STE100 standard.
