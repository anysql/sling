# SLING - A natural language frame semantics parser

[![Build Status](https://travis-ci.org/google/sling.svg?branch=caspar)](https://travis-ci.org/google/sling)

SLING CASPAR is a parser for annotating text with frame semantic annotations.
This is the second generation of the SLING parser. The first generation, SEMPAR,
can be found [here](https://github.com/google/sling). CASPAR is intended to
parse using cascades which are handling different parts of the transition
action space.

The basic CASPAR parser is a general transition-based frame semantic parser
using bi-directional LSTMs for input encoding and a Transition Based Recurrent
Unit (TBRU) for output decoding. It is a jointly trained model using only the
text tokens as input and the transition system has been designed to output frame
graphs directly without any intervening symbolic representation.

![SLING neural network architecture.](./doc/report/network.svg)

The SLING framework includes an efficient and scalable frame store
implementation as well as a neural network JIT compiler for fast parsing at
runtime.

A more detailed description of the SLING parser can be found in this paper:

* Michael Ringgaard, Rahul Gupta, and Fernando C. N. Pereira. 2017.
  *SLING: A framework for frame semantic parsing*. http://arxiv.org/abs/1710.07032.

</span>

## Trying out the parser

If you just want to try out the parser on a pre-trained model, you can install
the wheel with pip and download a pre-trained parser model. On a Linux machine
with Python 2.7 you can install a pre-built wheel:

```
sudo pip install http://www.jbox.dk/sling/sling-1.0.0-cp27-none-linux_x86_64.whl
```
and download the pre-trained model:
```
wget http://www.jbox.dk/sling/sempar.flow
```
You can then use the parser in Python:
```
import sling

parser = sling.Parser("sempar.flow")

text = raw_input("text: ")
doc = parser.parse(text)
print doc.frame.data(pretty=True)
for m in doc.mentions:
  print "mention", doc.phrase(m.begin, m.end)
```

## Installation

First, clone the GitHub repository and switch to the caspar branch.

```shell
git clone https://github.com/google/sling.git
cd sling
git checkout caspar
```

SLING uses [Bazel](https://bazel.build/) as the build system, so you need to
[install Bazel](https://docs.bazel.build/versions/master/install.html) in order
to build the SLING parser.

```shell
sudo apt-get install pkg-config zip g++ zlib1g-dev unzip python
wget -P /tmp https://github.com/bazelbuild/bazel/releases/download/0.13.0/bazel-0.13.0-installer-linux-x86_64.sh
chmod +x /tmp/bazel-0.13.0-installer-linux-x86_64.sh
sudo /tmp/bazel-0.13.0-installer-linux-x86_64.sh
```

The parser trainer uses Python v2.7 and PyTorch for training, so they need to be
installed.

```shell
# Change to your favorite version as needed.
sudo pip install http://download.pytorch.org/whl/cpu/torch-0.3.1-cp27-cp27mu-linux_x86_64.whl
```

## Building

Operating system: Linux<br>
Languages: C++, Python 2.7, assembler<br>
CPU: Intel x64 or compatible<br>
Build system: Bazel<br>

You can test your installation by building a few important targets.

```shell
git checkout caspar
bazel build -c opt sling/nlp/parser sling/nlp/parser/tools:all
```

Next, build and link to the SLING Python module since it will be used by the
trainer. But first, remember to switch to the caspar branch since it implements
all functionality inside CASPAR.

```shell
git checkout caspar
bazel build -c opt sling/pyapi:pysling.so
sudo ln -s $(realpath python) /usr/lib/python2.7/dist-packages/sling
```

**NOTE:**
*  In case you are using an older version of GCC (< v5), you may want to comment
out [this cxxopt](https://github.com/google/sling/blob/f8f0fbd1a18596ccfe6dbfba262a17afd36e2b5f/.bazelrc#L8) in .bazelrc.

## Training

Training a new model consists of preparing the commons store and the training
data, specifying various options and hyperparameters in the training script,
and tracking results as training progresses. These are described below in
detail.

### Data preparation

The first step consists of preparing the commons store (also called global store).
This has frame and schema definitions for all types and roles of interest, e.g.
`/saft/person` or `/pb/love-01` or `/pb/arg0`. In order to build the commons store
for the OntoNotes-based parser you need to checkout PropBank in a directory
parallel to the SLING directory:

```shell
cd ..
git clone https://github.com/propbank/propbank-frames.git propbank
cd sling
sling/nlp/parser/tools/build-commons.sh
```

This will build a SLING store with all the schemas needed and put it into
`/tmp/commons`.

Next, write a converter to convert documents in your existing format to
[SLING documents](sling/nlp/document/document.h). A SLING document is just a
document frame of type `/s/document`. An example of such a frame in textual encoding
can be seen below. It is best to create one SLING document per input sentence.

```shell
{
  :/s/document
  /s/document/text: "John loves Mary"
  /s/document/tokens: [
  {
      :/s/document/token
      /s/token/index: 0
      /s/token/start: 0
      /s/token/length: 4
      /s/token/break: 0
      /s/token/text: "John"
  },
  {
      :/s/document/token
      /s/token/index: 1
      /s/token/start: 5
      /s/token/length: 5
      /s/token/text: "loves"
  },
  {
      :/s/document/token
      /s/token/index: 2
      /s/token/start: 11
      /s/token/length: 4
      /s/token/text: "Mary"
  }]
  /s/document/mention: {=#1
    :/s/phrase
    /s/phrase/begin: 0
    /s/phrase/evokes: {=#2 :/saft/person }
  }
  /s/document/mention: {=#3
    :/s/phrase
    /s/phrase/begin: 1
    /s/phrase/evokes: {=#4
      :/pb/love-01
      /pb/arg0: #2
      /pb/arg1: {=#5 :/saft/person }
    }
  }
  /s/document/mention: {=#6
    :/s/phrase
    /s/phrase/begin: 2
    /s/phrase/evokes: #5
  }
}
```
For writing your converter or getting a better hold of the concepts of frames and store in SLING, you can have a look at detailed deep dive on frames and stores [here](sling/frame/README.md).

The SLING [Document class](sling/nlp/document/document.h)
also has methods to incrementally make such document frames, e.g.
```c++
Store global;
// Read global store from a file via LoadStore().

// Lookup handles in advance.
Handle h_person = global.Lookup("/saft/person");
Handle h_love01 = global.Lookup("/pb/love-01");
Handle h_arg0 = global.Lookup("/pb/arg0");
Handle h_arg1 = global.Lookup("/pb/arg1");

// Prepare the document.
Store store(&global);
Document doc(&store);  // empty document

// Add token information.
doc.SetText("John loves Mary");
doc.AddToken(0, 4, "John", 0);
doc.AddToken(5, 10, "loves", 1);
doc.AddToken(11, 15, "Mary", 1);

// Create frames that will eventually be evoked.
Builder b1(&store);
b1.AddIsA(h_person);
Frame john_frame = b1.Create();

Builder b2(&store);
b2.AddIsA(h_person);
Frame mary_frame = b2.Create();

Builder b3(&store);
b3.AddIsA(h_love01);
b3.Add(h_arg0, john_frame);
b3.Add(h_arg1, mary_frame);
Frame love_frame = b3.Create();

# Add spans and evoke frames from them.
doc.AddSpan(0, 1)->Evoke(john_frame);
doc.AddSpan(1, 2)->Evoke(love_frame);
doc.AddSpan(2, 3)->Evoke(mary_frame);

doc.Update();
string encoded = Encode(doc.top());

// Append 'encoded' to a recordio file.
RecordWriter writer(<filename>);

writer.Write(encoded);
...<write more documents>

writer.Close();
```

Use the converter to create the following corpora:
+ Training corpus of annotated SLING documents.
+ Dev corpus of annotated SLING documents.

CASPAR uses the [recordio file format](https://github.com/google/sling/blob/caspar/sling/file/recordio.h)
for training where each record corresponds to one encoded document. This format is up to 25x faster
to read than zip files, with almost identical compression ratios.

### Specify training options and hyperparameters:

Once the commons store and the corpora have been built, you are ready for training
a model. For this, use the supplied [training script](sling/nlp/parser/tools/train.sh).
The script provides various commandline arguments. The ones that specify
the input data are:
+ `--commons`: File path of the commons store built in the previous step.
+ `--train`: Path to the training corpus built in the previous step.
+ `--dev`: Path to the annotated dev corpus built in the previous step.
+ `--output` or `--output_dir`: Output folder where checkpoints, master spec,
  temporary files, and the final model will be saved.

Then we have the various training options and hyperparameters:
+ `--word_embeddings`: Empty, or path to pretrained word embeddings in
  [Mikolov's word2vec format](https://github.com/tmikolov/word2vec/blob/master/word2vec.c).
  If supplied, these are used to initialize the embeddings for word features.
+ `--batch`: Batch size used during training.
+ `--report_every`: Checkpoint interval (in number of batches).
+ `--steps`: Number of training batches to process.
+ `--method`: Optimization method to use (e.g. adam or momentum), along
  with auxiliary arguments like `--adam_beta1`, `--adam_beta2`, `--adam_eps`.
+ `--learning_rate`: Learning rate.
+ `--grad_clip_norm`: Max norm beyond which gradients will be clipped.
+ `--moving_average`: Whether or not to use exponential moving average.

The script comes with reasonable defaults for the hyperparameters for
training a semantic parser model, but it would be a good idea to hardcode
your favorite arguments [directly in the
flag definitions](sling/nlp/parser/trainer/train_util.py#L94)
to avoid supplying them again and again on the commandline.

### Run the training script

To test your training setup, you can kick off a small training run:
```shell
./sling/nlp/parser/tools/train.sh --commons=<path to commons> \
   --train=<oath to train recordio> --dev=<path to dev recordio> \
   --report_every=500 --train_steps=1000 --output=<output folder>
```

This training run should be over in 10-20 minutes, and should checkpoint and
evaluate after every 500 steps. For a full-training run, we suggest increasing
the number of steps to something like 100,000 and decreasing the checkpoint
frequency to something like every 2000-5000 steps.

As training proceeds, the training script produces a lot of useful
diagnostic information, which is logged by default to a file called "log"
inside the specified output folder.

* The script will first generate the PyTorch model and print its specification, i.e. various
sub-modules inside the model and their dimensionalities.

```shell
Modules: Sempar(
  (lr_lstm_embedding_words): EmbeddingBag(53257, 32, mode=sum)
  (rl_lstm_embedding_words): EmbeddingBag(53257, 32, mode=sum)
  (lr_lstm_embedding_suffix): EmbeddingBag(8334, 16, mode=sum)
  (rl_lstm_embedding_suffix): EmbeddingBag(8334, 16, mode=sum)
  (lr_lstm_embedding_capitalization): EmbeddingBag(5, 8, mode=sum)
  (rl_lstm_embedding_capitalization): EmbeddingBag(5, 8, mode=sum)
  (lr_lstm_embedding_hyphen): EmbeddingBag(2, 8, mode=sum)
  (rl_lstm_embedding_hyphen): EmbeddingBag(2, 8, mode=sum)
  (lr_lstm_embedding_punctuation): EmbeddingBag(3, 8, mode=sum)
  (rl_lstm_embedding_punctuation): EmbeddingBag(3, 8, mode=sum)
  (lr_lstm_embedding_quote): EmbeddingBag(4, 8, mode=sum)
  (rl_lstm_embedding_quote): EmbeddingBag(4, 8, mode=sum)
  (lr_lstm_embedding_digit): EmbeddingBag(3, 8, mode=sum)
  (rl_lstm_embedding_digit): EmbeddingBag(3, 8, mode=sum)
  (lr_lstm): DragnnLSTM(in=88, hidden=256)
  (rl_lstm): DragnnLSTM(in=88, hidden=256)
  (ff_fixed_embedding_in-roles): EmbeddingBag(125, 16, mode=sum)
  (ff_fixed_embedding_out-roles): EmbeddingBag(125, 16, mode=sum)
  (ff_fixed_embedding_labeled-roles): EmbeddingBag(625, 16, mode=sum)
  (ff_fixed_embedding_unlabeled-roles): EmbeddingBag(25, 16, mode=sum)
  (ff_link_transform_frame-creation-steps): LinkTransform(input_activation=128, dim=64, oov_vector=64)
  (ff_link_transform_frame-focus-steps): LinkTransform(input_activation=128, dim=64, oov_vector=64)
  (ff_link_transform_frame-end-lr): LinkTransform(input_activation=256, dim=32, oov_vector=32)
  (ff_link_transform_frame-end-rl): LinkTransform(input_activation=256, dim=32, oov_vector=32)
  (ff_link_transform_history): LinkTransform(input_activation=128, dim=64, oov_vector=64)
  (ff_link_transform_lr): LinkTransform(input_activation=256, dim=32, oov_vector=32)
  (ff_link_transform_rl): LinkTransform(input_activation=256, dim=32, oov_vector=32)
  (ff_layer): Projection(in=1344, out=128, bias=True)
  (ff_relu): ReLU()
  (ff_softmax): Projection(in=128, out=6968, bias=True)
  (loss_fn): CrossEntropyLoss(
  )
)

```

* Training will now commence, and you will see the training cost being logged at regular intervals.
  ```shell
  BatchLoss after (1 batches = 8 examples): 2.94969940186  incl. L2= [0.000000029] (1.6 secs) <snip>
  BatchLoss after (2 batches = 16 examples): 2.94627690315  incl. L2= [0.000000029] (1.9 secs) <snip>
  BatchLoss after (3 batches = 24 examples): 2.94153237343  incl. L2= [0.000000037] (1.1 secs) <snip>
  ...
  <snip>

  ```
* After every checkpoint interval (specified via `--report_every`),
  it will save the model and evaluate it on the dev corpus.
  The evaluation runs a [graph matching algorithm](sling/nlp/parser/trainer/frame-evaluation.h)
  that outputs various metrics from aligning the gold frame graph
  vs the test frame graph. If you are looking for a single number to
  quantify your model, then we suggest using **SLOT_F1**, which aggregates across
  frame type and role accuracies (i.e. both node and edge alignment scores).

  Note that graph matching is an intrinsic evaluation, so if you wish to swap
  it with an extrinsic evaluation, then just replace the binary
  [here](sling/nlp/parser/trainer/train_util.py#L30) with your evaluation binary.

* At any point, the best performing checkpoint will be available as a Myelin flow file
  in `<output folder>/pytorch.best.flow`.

**NOTE:**
* If you wish to modify the default set of features,
then you would have to modify the [feature specification code](sling/nlp/parser/trainer/spec.py#L212).

## Parsing

The trained parser model is stored in a [Myelin](sling/myelin/README.md) flow file,
It contains all the information needed for parsing text:
* The neural network units (LR, RL, FF) with the parameters learned from
training.
* Feature maps for the lexicon and affixes.
* The commons store is a [SLING store](sling/frame/README.md) with the schemas for the
frames.
* The action table with all the transition actions.

A pre-trained model can be downloaded from [here](http://www.jbox.dk/sling/sempar.flow).
The model can be loaded and initialized in the following way:

```c++
#include "sling/frame/store.h"
#include "sling/nlp/document/document-tokenizer.h"
#include "sling/nlp/parser/parser.h"

// Load parser model.
sling::Store commons;
sling::nlp::Parser parser;
parser.Load(&commons, "/tmp/sempar.flow");
commons.Freeze();

// Create document tokenizer.
sling::nlp::DocumentTokenizer tokenizer;
```

In order to parse some text, it first needs to be tokenized. The document with
text, tokens, and frames is stored in a local document frame store.

```c++
// Create frame store for document.
sling::Store store(&commons);
sling::nlp::Document document(&store);

// Tokenize text.
string text = "John hit the ball with a bat.";
tokenizer.Tokenize(&document, text);

// Parse document.
parser.Parse(&document);
document.Update();

// Output document annotations.
std::cout << sling::ToText(document.top(), 2);
```

## Myelin-based parser tool

SLING comes with a [parsing tool](sling/nlp/parser/tools/parse.cc)
for annotating a corpus of documents with frames
using a parser model, benchmarking this annotation process, and optionally
evaluating the annotated frames against supplied gold frames.


This tool takes the following commandline arguments:

*  `--parser` : This should point to a Myelin flow, e.g. one created by the
   training script.
*  If `--text` is specified then the parser is run over the supplied text, and
   prints the annotated frame(s) in text mode. The indentation of the text
   output can be controlled by `--indent`. E.g.
   ```shell
   bazel build -c opt sling/nlp/parser/tools:parse
   bazel-bin/sling/nlp/parser/tools/parse --logtostderr \
      --parser=<path to flow file> --text="John loves Mary" --indent=2

   {=#1
     :/s/document
     /s/document/text: "John loves Mary"
     /s/document/tokens: [{=#2
       :/s/token
       /s/token/index: 0
       /s/token/text: "John"
       /s/token/start: 0
       /s/token/length: 4
       /s/token/break: 0
     }, {=#3
       :/s/token
       /s/token/index: 1
       /s/token/text: "loves"
       /s/token/start: 5
       /s/token/length: 5
     }, {=#4
       :/s/token
       /s/token/index: 2
       /s/token/text: "Mary"
       /s/token/start: 11
       /s/token/length: 4
     }]
     /s/document/mention: {=#5
       :/s/phrase
       /s/phrase/begin: 0
       /s/phrase/evokes: {=#6
         :/saft/person
       }
     }
     /s/document/mention: {=#7
       :/s/phrase
       /s/phrase/begin: 1
       /s/phrase/evokes: {=#8
         :/pb/love-01
         /pb/arg0: #6
         /pb/arg1: {=#9
           :/saft/person
         }
       }
     }
     /s/document/mention: {=#10
       :/s/phrase
       /s/phrase/begin: 2
       /s/phrase/evokes: #9
     }
   }
   I0927 14:44:25.705880 30901 parse.cc:154] 823.732 tokens/sec
   ```
*  If `--benchmark` is specified then the parser is run on the document
   corpus specified via `--corpus`. This corpus should be prepared similarly to
   how the training/dev corpora were created. The processing can be limited to
   the first N documents by specifying `--maxdocs=N`.

   ```shell
    bazel-bin/sling/nlp/parser/tools/parse --logtostderr \
      --parser=sempar.flow --corpus=dev.zip -benchmark --maxdocs=200

    I0927 14:45:36.634670 30934 parse.cc:127] Load parser from sempar.flow
    I0927 14:45:37.307870 30934 parse.cc:135] 565.077 ms loading parser
    I0927 14:45:37.307922 30934 parse.cc:161] Benchmarking parser on dev.zip
    I0927 14:45:39.059257 30934 parse.cc:184] 200 documents, 3369 tokens, 2289.91 tokens/sec
   ```

   If `--profile` is specified, the parser will run with profiling
   instrumentation enabled and output a detailed profile report with execution
   timing for each operation in the neural network.

*  If `--evaluate` is specified then the tool expects `--corpora` to specify
   a corpora with gold frames. It then runs the parser model over a frame-less
   version of this corpora and evaluates the annotated frames vs the gold
   frames. Again, one can use `--maxdocs` to limit the evaluation to the first N
   documents.
   ```shell
   bazel-bin/sling/nlp/parser/tools/parse --logtostderr \
     --evaluate --parser=sempar.flow --corpus=dev.rec --maxdocs=200

   I0927 14:51:39.542151 31336 parse.cc:127] Load parser from sempar.flow
   I0927 14:51:40.211920 31336 parse.cc:135] 562.249 ms loading parser
   I0927 14:51:40.211973 31336 parse.cc:194] Evaluating parser on dev.rec
   SPAN_P+ 1442
   SPAN_P- 93
   SPAN_R+ 1442
   SPAN_R- 133
   SPAN_Precision  93.941368078175884
   SPAN_Recall     91.555555555555557
   SPAN_F1 92.733118971061089
   ...
   <snip>
   ...
   SLOT_F1 78.398993883366586
   COMBINED_P+     4920
   COMBINED_P-     633
   COMBINED_R+     4923
   COMBINED_R-     901
   COMBINED_Precision      88.60075634792004
   COMBINED_Recall 84.529532967032978
   COMBINED_F1     86.517276488704127
   ```

## Credits

Original authors of the code in this package include:

*   Michael Ringgaard
*   Rahul Gupta



