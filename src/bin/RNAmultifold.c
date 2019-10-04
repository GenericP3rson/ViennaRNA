/*
 *                c Ronny Lorenz, ViennaRNA Package
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>

#include "ViennaRNA/plotting/probabilities.h"
#include "ViennaRNA/plotting/structures.h"
#include "ViennaRNA/fold_vars.h"
#include "ViennaRNA/params/basic.h"
#include "ViennaRNA/commands.h"
#include "ViennaRNA/constraints/basic.h"
#include "ViennaRNA/constraints/hard.h"
#include "ViennaRNA/constraints/soft.h"
#include "ViennaRNA/constraints/SHAPE.h"
#include "ViennaRNA/io/file_formats.h"
#include "ViennaRNA/io/utils.h"
#include "ViennaRNA/cofold.h"
#include "ViennaRNA/fold.h"
#include "ViennaRNA/part_func_co.h"
#include "ViennaRNA/part_func.h"
#include "ViennaRNA/centroid.h"
#include "ViennaRNA/MEA.h"
#include "ViennaRNA/utils/basic.h"
#include "ViennaRNA/utils/strings.h"
#include "ViennaRNA/utils/structures.h"
#include "ViennaRNA/params/io.h"
#include "ViennaRNA/datastructures/char_stream.h"
#include "ViennaRNA/datastructures/stream_output.h"

#include "RNAmultifold_cmdl.h"
#include "gengetopt_helper.h"
#include "input_id_helpers.h"
#include "ViennaRNA/color_output.inc"
#include "parallel_helpers.h"


struct options {
  int             filename_full;
  char            *filename_delim;
  int             pf;
  int             doT;
  int             doC;
  int             noPS;
  int             noconv;
  int             centroid;
  int             MEA;
  double          MEAgamma;
  double          bppmThreshold;
  int             verbose;
  vrna_md_t       md;
  vrna_cmd_t      commands;

  dataset_id      id_control;

  char            *concentration_file;

  char            *constraint_file;
  int             constraint_batch;
  int             constraint_enforce;
  int             constraint_canonical;

  int             shape;
  char            *shape_file;
  char            *shape_method;
  char            *shape_conversion;

  int             csv_output;
  int             csv_header;
  char            csv_output_delim;

  int             jobs;
  int             keep_order;
  unsigned int    next_record_number;
  vrna_ostream_t  output_queue;
};


struct record_data {
  unsigned int    number;
  char            *id;
  char            *sequence;
  char            *SEQ_ID;
  char            **rest;
  char            *input_filename;
  int             multiline_input;
  struct options  *options;
  int             tty;
};


struct output_stream {
  vrna_cstr_t data;
  vrna_cstr_t err;
};


static void
process_record(struct record_data *record);


PRIVATE vrna_dimer_pf_t
do_partfunc(char            *string,
            int             length,
            int             Switch,
            plist           **tpr,
            plist           **mf,
            double          kT,
            struct options  *opt);


PRIVATE double *
read_concentrations(FILE *fp);


PRIVATE void
print_concentrations(vrna_cstr_t        stream,
                     vrna_dimer_conc_t  *result,
                     double             *startconc);


static int
process_input(FILE            *input_stream,
              const char      *input_filename,
              struct options  *opt);


static void
write_csv_header(FILE           *stream,
                 struct options *opt);


void
postscript_layout(vrna_fold_compound_t  *fc,
                  const char            *orig_sequence,
                  const char            *structure,
                  const char            *SEQ_ID,
                  struct options        *opt);


static char *
get_filename(const char     *id,
             const char     *suffix,
             const char     *filename_default,
             struct options *opt);


static void
compute_centroid(vrna_fold_compound_t *fc,
                 vrna_cstr_t          rec_output);


static void
compute_MEA(vrna_fold_compound_t  *fc,
            double                MEAgamma,
            vrna_cstr_t           rec_output);


/*--------------------------------------------------------------------------*/

void
init_default_options(struct options *opt)
{
  opt->filename_full  = 0;
  opt->filename_delim = NULL;
  opt->pf             = 0;
  opt->doT            = 0; /* compute dimer free energies etc. */
  opt->noPS           = 0;
  opt->noconv         = 0;
  opt->centroid       = 0;  /* off by default due to historical reasons */
  opt->MEA            = 0;
  opt->MEAgamma       = 1.;
  opt->bppmThreshold  = 1e-5;
  opt->verbose        = 0;
  opt->commands       = NULL;
  opt->id_control     = NULL;
  set_model_details(&(opt->md));
  //opt->md.pf_smooth  = 0;

  opt->doC                = 0; /* toggle to compute concentrations */
  opt->concentration_file = NULL;

  opt->constraint_file      = NULL;
  opt->constraint_batch     = 0;
  opt->constraint_enforce   = 0;
  opt->constraint_canonical = 0;

  opt->shape            = 0;
  opt->shape_file       = NULL;
  opt->shape_method     = NULL;
  opt->shape_conversion = NULL;

  opt->csv_output       = 0;    /* flag indicating whether we produce one-line outputs, a.k.a. CSV */
  opt->csv_header       = 1;    /* print header for one-line output */
  opt->csv_output_delim = ',';  /* delimiting character for one-line output */

  opt->jobs               = 1;
  opt->keep_order         = 1;
  opt->next_record_number = 0;
  opt->output_queue       = NULL;
}


static char **
collect_unnamed_options(struct RNAmultifold_args_info  *ggostruct,
                        int                         *num_files)
{
  char  **input_files = NULL;
  int   i;

  *num_files = 0;

  /* collect all unnamed options */
  if (ggostruct->inputs_num > 0) {
    input_files = (char **)vrna_realloc(input_files, sizeof(char *) * ggostruct->inputs_num);
    for (i = 0; i < ggostruct->inputs_num; i++)
      input_files[(*num_files)++] = strdup(ggostruct->inputs[i]);
  }

  return input_files;
}


void
flush_cstr_callback(void          *auxdata,
                    unsigned int  i,
                    void          *data)
{
  struct output_stream *s = (struct output_stream *)data;

  /* flush/free errors first */
  vrna_cstr_free(s->err);

  /* flush/free data[k] */
  vrna_cstr_free(s->data);

  free(s);
}


int
main(int  argc,
     char *argv[])
{
  struct  RNAmultifold_args_info args_info;
  char                        **input_files;
  int                         num_input;
  struct  options             opt;

  num_input = 0;

  init_default_options(&opt);

  /*
   #############################################
   # check the command line parameters
   #############################################
   */
  if (RNAmultifold_cmdline_parser(argc, argv, &args_info) != 0)
    exit(1);

  /* get basic set of model details */
  ggo_get_md_eval(args_info, opt.md);
  ggo_get_md_fold(args_info, opt.md);
  ggo_get_md_part(args_info, opt.md);

  /* temperature */
  ggo_get_temperature(args_info, opt.md.temperature);

  /* check dangle model */
  if ((opt.md.dangles < 0) || (opt.md.dangles > 3)) {
    vrna_message_warning("required dangle model not implemented, falling back to default dangles=2");
    opt.md.dangles = dangles = 2;
  }

  /* SHAPE reactivity data */
  ggo_get_SHAPE(args_info, opt.shape, opt.shape_file, opt.shape_method, opt.shape_conversion);

  ggo_get_id_control(args_info, opt.id_control, "Sequence", "sequence", "_", 4, 1);

  ggo_get_constraints_settings(args_info,
                               fold_constrained,
                               opt.constraint_file,
                               opt.constraint_enforce,
                               opt.constraint_batch);

  /* enforce canonical base pairs in any case? */
  if (args_info.canonicalBPonly_given)
    opt.constraint_canonical = 1;

  /* do not convert DNA nucleotide "T" to appropriate RNA "U" */
  if (args_info.noconv_given)
    opt.noconv = 1;

  /*  */
  if (args_info.noPS_given)
    opt.noPS = 1;

  /* concentrations from stdin */
  if (args_info.concentrations_given)
    opt.doC = opt.doT = opt.pf = 1;

  /* set the bppm threshold for the dotplot */
  if (args_info.bppmThreshold_given)
    opt.bppmThreshold = MIN2(1., MAX2(0., args_info.bppmThreshold_arg));

  /* concentrations in file */
  if (args_info.concfile_given) {
    opt.concentration_file  = strdup(args_info.concfile_arg);
    opt.doC                 = opt.doT = opt.pf = 1;
  }

  /* partition function settings */
  if (args_info.partfunc_given) {
    opt.pf = 1;
    if (args_info.partfunc_arg != -1)
      opt.md.compute_bpp = args_info.partfunc_arg;
  }

  if (args_info.all_pf_given) {
    opt.doT = opt.pf = 1;
    if (args_info.all_pf_arg != 1)
      opt.md.compute_bpp = args_info.all_pf_arg;
    else
      opt.md.compute_bpp = 1;
  }

  /* MEA (maximum expected accuracy) settings */
  if (args_info.MEA_given) {
    opt.MEA = 1;

    if (args_info.MEA_arg != -1)
      opt.MEAgamma = args_info.MEA_arg;

    if (!opt.pf)
      opt.pf = 1;

    if (!opt.md.compute_bpp)
      opt.md.compute_bpp = 1;
  }

  if (args_info.centroid_given) {
    opt.centroid = 1;

    if (!opt.pf)
      opt.pf = 1;

    if (!opt.md.compute_bpp)
      opt.md.compute_bpp = 1;
  }

  if (args_info.verbose_given)
    opt.verbose = 1;

  if (args_info.commands_given)
    opt.commands = vrna_file_commands_read(args_info.commands_arg,
                                           VRNA_CMD_PARSE_HC | VRNA_CMD_PARSE_SC);

  /* filename sanitize delimiter */
  if (args_info.filename_delim_given)
    opt.filename_delim = strdup(args_info.filename_delim_arg);
  else if (get_id_delim(opt.id_control))
    opt.filename_delim = strdup(get_id_delim(opt.id_control));

  if ((opt.filename_delim) && isspace(*(opt.filename_delim))) {
    free(opt.filename_delim);
    opt.filename_delim = NULL;
  }

  /* full filename from FASTA header support */
  if (args_info.filename_full_given)
    opt.filename_full = 1;

  /* output format changes */
  if (args_info.output_format_given) {
    switch (*(args_info.output_format_arg)) {
      case 'D':
      /* fall-through */
      case 'd':
        opt.csv_output = 1;
        break;
      case 'V':
      /* fall-through */
      case 'v':
        opt.csv_output = 0;
        break;
      default:
        vrna_message_warning("unknown output format \"%c\", using defaults!",
                             *(args_info.output_format_arg));
        break;
    }
  }

  /* one-line output delimiter */
  if (args_info.csv_delim_given) {
    opt.csv_output_delim = *(args_info.csv_delim_arg);
    if (!opt.csv_output_delim) {
      vrna_message_warning("Delimiting character for One-Line output is missing, using defaults!");
      opt.csv_output_delim = ',';
    }
  }

  /* one-line output header */
  if (args_info.csv_noheader_given)
    opt.csv_header = 0;

  if (args_info.jobs_given) {
#if VRNA_WITH_PTHREADS
    int thread_max = max_user_threads();
    if (args_info.jobs_arg == 0) {
      /* use maximum of concurrent threads */
      int proc_cores, proc_cores_conf;
      if (num_proc_cores(&proc_cores, &proc_cores_conf)) {
        opt.jobs = MIN2(thread_max, proc_cores_conf);
      } else {
        vrna_message_warning("Could not determine number of available processor cores!\n"
                             "Defaulting to serial computation");
        opt.jobs = 1;
      }
    } else {
      opt.jobs = MIN2(thread_max, args_info.jobs_arg);
    }

    opt.jobs = MAX2(1, opt.jobs);
#else
    vrna_message_warning(
      "This version of RNAfold has been built without parallel input processing capabilities");
#endif

    if (args_info.unordered_given)
      opt.keep_order = 0;
  }

  input_files = collect_unnamed_options(&args_info, &num_input);

  /* free allocated memory of command line data structure */
  RNAmultifold_cmdline_parser_free(&args_info);

  /*
   #############################################
   # begin initializing
   #############################################
   */
  if (opt.pf && opt.md.gquad)
    vrna_message_error(
      "G-Quadruplex support is currently not available for partition function computations");

  if ((opt.csv_output) && (opt.csv_header))
    write_csv_header(stdout, &opt);

  if ((opt.verbose) && (opt.jobs > 1))
    vrna_message_info(stderr, "Preparing %d parallel computation slots", opt.jobs);

  if (opt.keep_order)
    opt.output_queue = vrna_ostream_init(&flush_cstr_callback, NULL);

  /*
   ################################################
   # process input files or handle input from stdin
   ################################################
   */
  INIT_PARALLELIZATION(opt.jobs);

  if (num_input > 0) {
    int i, skip;
    for (skip = i = 0; i < num_input; i++) {
      if (!skip) {
        FILE *input_stream = fopen((const char *)input_files[i], "r");

        if (!input_stream)
          vrna_message_error("Unable to open %d. input file \"%s\" for reading", i + 1,
                             input_files[i]);

        if (opt.verbose) {
          vrna_message_info(stderr,
                            "Processing %d. input file \"%s\"",
                            i + 1,
                            input_files[i]);
        }

        if (process_input(input_stream, (const char *)input_files[i], &opt) == 0)
          skip = 1;

        fclose(input_stream);
      }

      free(input_files[i]);
    }
  } else {
    (void)process_input(stdin, NULL, &opt);
  }

  UNINIT_PARALLELIZATION
  /*
   ################################################
   # post processing
   ################################################
   */
  vrna_ostream_free(opt.output_queue);


  free(input_files);
  free(opt.constraint_file);
  free(opt.shape_file);
  free(opt.shape_method);
  free(opt.shape_conversion);
  free(opt.filename_delim);
  vrna_commands_free(opt.commands);
  free(opt.concentration_file);

  free_id_data(opt.id_control);

  return EXIT_SUCCESS;
}


static int
process_input(FILE            *input_stream,
              const char      *input_filename,
              struct options  *opt)
{
  unsigned int  read_opt;
  int           istty, istty_in, istty_out, ret;

  ret       = 1;
  read_opt  = 0;

  istty_in  = isatty(fileno(input_stream));
  istty_out = isatty(fileno(stdout));
  istty     = istty_in && istty_out;

  /* print user help if we get input from tty */
  if (istty) {
    if (fold_constrained) {
      vrna_message_constraint_options(
        VRNA_CONSTRAINT_DB_DOT | VRNA_CONSTRAINT_DB_X | VRNA_CONSTRAINT_DB_ANG_BRACK |
        VRNA_CONSTRAINT_DB_RND_BRACK);
      vrna_message_input_seq("Input sequence (upper or lower case) followed by structure constraint\n"
                             "Use '&' to connect 2 sequences that shall form a complex.");
    } else {
      vrna_message_input_seq("Use '&' to connect 2 sequences that shall form a complex.");
    }
  }

  /* set options we wanna pass to vrna_file_fasta_read_record() */
  if (istty)
    read_opt |= VRNA_INPUT_NOSKIP_BLANK_LINES;

  if (!fold_constrained)
    read_opt |= VRNA_INPUT_NO_REST;

  /*
   #############################################
   # main loop: continue until end of file
   #############################################
   */
  do {
    char          *rec_sequence, *rec_id, **rec_rest;
    unsigned int  rec_type;
    int           maybe_multiline;

    rec_id          = NULL;
    rec_rest        = NULL;
    maybe_multiline = 0;

    rec_type = vrna_file_fasta_read_record(&rec_id,
                                           &rec_sequence,
                                           &rec_rest,
                                           input_stream,
                                           read_opt);

    if (rec_type & (VRNA_INPUT_ERROR | VRNA_INPUT_QUIT))
      break;

    /*
     ########################################################
     # init everything according to the data we've read
     ########################################################
     */
    if (rec_id) {
      maybe_multiline = 1;
      /* remove '>' from FASTA header */
      rec_id = memmove(rec_id, rec_id + 1, strlen(rec_id));
    }

    /* construct the sequence ID */
    set_next_id(&rec_id, opt->id_control);

    struct record_data *record = (struct record_data *)vrna_alloc(sizeof(struct record_data));

    record->number          = opt->next_record_number;
    record->sequence        = rec_sequence;
    record->SEQ_ID          = fileprefix_from_id(rec_id, opt->id_control, opt->filename_full);
    record->id              = rec_id;
    record->rest            = rec_rest;
    record->multiline_input = maybe_multiline;
    record->options         = opt;
    record->tty             = istty;
    record->input_filename  = (input_filename) ? strdup(input_filename) : NULL;

    if (opt->output_queue)
      vrna_ostream_request(opt->output_queue, opt->next_record_number++);

    RUN_IN_PARALLEL(process_record, record);

    if (opt->shape || (opt->constraint_file && (!opt->constraint_batch))) {
      ret = 0;
      break;
    }

    /* print user help for the next round if we get input from tty */
    if (istty) {
      printf("Use '&' to connect 2 sequences that shall form a complex.\n");
      if (fold_constrained) {
        vrna_message_constraint_options(
          VRNA_CONSTRAINT_DB_DOT | VRNA_CONSTRAINT_DB_X | VRNA_CONSTRAINT_DB_ANG_BRACK |
          VRNA_CONSTRAINT_DB_RND_BRACK);
        vrna_message_input_seq(
          "Input sequence (upper or lower case) followed by structure constraint\n");
      } else {
        vrna_message_input_seq_simple();
      }
    }
  } while (1);

  return ret;
}


static void
process_record(struct record_data *record)
{
  char                  *mfe_structure, *sequence, **rec_rest;
  unsigned int          n, i;
  double                min_en, kT, *concentrations;
  vrna_ep_t             *prAB, *prAA, *prBB, *prA, *prB, *mfAB, *mfAA, *mfBB, *mfA, *mfB;
  struct options        *opt;
  struct output_stream  *o_stream;

  mfAB            = mfAA = mfBB = mfA = mfB = NULL;
  prAB            = prAA = prBB = prA = prB = NULL;
  concentrations  = NULL;
  opt             = record->options;
  o_stream        = (struct output_stream *)vrna_alloc(sizeof(struct output_stream));
  sequence        = strdup(record->sequence);
  rec_rest        = record->rest;

  /* convert DNA alphabet to RNA if not explicitely switched off */
  if (!opt->noconv) {
    vrna_seq_toRNA(sequence);
    vrna_seq_toRNA(record->sequence);
  }

  /* convert sequence to uppercase letters only */
  vrna_seq_toupper(sequence);

  vrna_fold_compound_t *vc = vrna_fold_compound(sequence,
                                                &(opt->md),
                                                VRNA_OPTION_DEFAULT | VRNA_OPTION_HYBRID);
  n = vc->length;

  /* retrieve string stream bound to stdout, 6*length should be enough memory to start with */
  o_stream->data = vrna_cstr(6 * n, stdout);
  /* retrieve string stream bound to stderr for any info messages */
  o_stream->err = vrna_cstr(n, stderr);

  if (record->tty) {
    if (vc->cutpoint == -1) {
      vrna_message_info(stdout, "length = %u", n);
    } else {
      vrna_message_info(stdout,
                        "length1 = %d\nlength2 = %d",
                        vc->cutpoint - 1,
                        (int)n - vc->cutpoint + 1);
    }
  }

  if (vc->cutpoint == vc->length / 2 + 1) {
    if (!strncmp(vc->sequence, vc->sequence + vc->cutpoint - 1, vc->cutpoint - 1)) {
      vrna_cstr_message_warning(o_stream->err,
                                "Both input strands are identical, thus inducing rotationally symmetry! "
                                "Symmetry correction might be required to compute actual MFE!");
    }
  }

  mfe_structure = (char *)vrna_alloc(sizeof(char) * (n + 1));

  /* parse the rest of the current dataset to obtain a structure constraint */
  if (fold_constrained) {
    if (opt->constraint_file) {
      vrna_constraints_add(vc, opt->constraint_file, VRNA_OPTION_DEFAULT | VRNA_OPTION_HYBRID);
    } else {
      char          *cstruc   = NULL;
      unsigned int  cl        = 0;
      unsigned int  coptions  = (record->multiline_input) ? VRNA_OPTION_MULTILINE : 0;
      int           cp        = -1;

      cstruc  = vrna_extract_record_rest_structure((const char **)rec_rest, 0, coptions);
      cstruc  = vrna_cut_point_remove(cstruc, &cp);
      if (vc->cutpoint != cp) {
        vrna_message_error("Sequence and Structure have different cut points.\n"
                           "sequence: %d, structure: %d",
                           vc->cutpoint, cp);
      }

      cl = (cstruc) ? (int)strlen(cstruc) : 0;

      if (cl == 0)
        vrna_message_warning("Structure constraint is missing");
      else if (cl < n)
        vrna_message_warning("Structure constraint is shorter than sequence");
      else if (cl > n)
        vrna_message_error("Structure constraint is too long");

      if (cstruc) {
        unsigned int constraint_options = VRNA_CONSTRAINT_DB_DEFAULT;

        if (opt->constraint_enforce)
          constraint_options |= VRNA_CONSTRAINT_DB_ENFORCE_BP;

        if (opt->constraint_canonical)
          constraint_options |= VRNA_CONSTRAINT_DB_CANONICAL_BP;

        vrna_constraints_add(vc, (const char *)cstruc, constraint_options);
      }

      free(cstruc);
    }
  }

  if (opt->shape) {
    vrna_constraints_add_SHAPE(vc,
                               opt->shape_file,
                               opt->shape_method,
                               opt->shape_conversion,
                               opt->verbose,
                               VRNA_OPTION_DEFAULT | VRNA_OPTION_HYBRID);
  }

  if (opt->commands)
    vrna_commands_apply(vc, opt->commands, VRNA_CMD_PARSE_HC | VRNA_CMD_PARSE_SC);

  if (opt->doC) {
    if (opt->concentration_file) {
      /* read from file */
      FILE *fp = fopen(opt->concentration_file, "r");
      if (fp == NULL)
        vrna_message_error("could not open concentration file %s", opt->concentration_file);

      concentrations = read_concentrations(fp);
      fclose(fp);
    } else {
      printf("Please enter concentrations [mol/l]\n format: ConcA ConcB\n return to end\n");
      concentrations = read_concentrations(stdin);
    }
  }

  /*
   ########################################################
   # begin actual computations
   ########################################################
   */

  /* compute mfe of AB dimer */
  for (i = 0; i < n; i++)
    mfe_structure[i] = '.';

  vc->params->model_details.backtrack = 0;
  min_en  = vrna_mfe_dimer(vc, mfe_structure);
  mfAB    = vrna_plist(mfe_structure, 0.95);
  vc->params->model_details.backtrack = 1;

  /* check whether the constraint allows for any solution */
  if ((fold_constrained) || (opt->commands)) {
    if (min_en == (double)(INF / 100.)) {
      vrna_message_error(
        "Supplied structure constraints create empty solution set for sequence:\n%s",
        record->sequence);
      exit(EXIT_FAILURE);
    }
  }

  {
    char *pstruct = vrna_cut_point_insert(mfe_structure, vc->cutpoint);

    if (opt->csv_output) {
      vrna_cstr_printf(o_stream->data,
                       "%ld%c"          /* sequence number */
                       "%s%c"           /* sequence id */
                       "\"%s\"%c"       /* sequence */
                       "\"%s\"%c"       /* MFE structure */
                       "%6.2f",         /* MFE */
                       get_current_id(opt->id_control), opt->csv_output_delim,
                       (record->id) ? record->id : "", opt->csv_output_delim,
                       record->sequence, opt->csv_output_delim,
                       (pstruct) ? pstruct : "", opt->csv_output_delim,
                       min_en);
    } else {
      vrna_cstr_print_fasta_header(o_stream->data, record->id);
      vrna_cstr_printf(o_stream->data, "%s\n", record->sequence);
      vrna_cstr_printf_structure(o_stream->data,
                                 pstruct,
                                 record->tty ? "\n minimum free energy = %6.2f kcal/mol" : " (%6.2f)",
                                 min_en);
    }

    if (!opt->noPS)
      postscript_layout(vc, record->sequence, pstruct, record->SEQ_ID, opt);

    free(pstruct);
  }

  if (n > 2000)
    vrna_mx_mfe_free(vc);

  /* compute partition function */
  if (opt->pf) {
    char              *pairing_propensity;
    double            dG;

    prAB                = NULL;
    pairing_propensity  = (char *)vrna_alloc(sizeof(char) * (n + 1));

    if (opt->md.dangles == 1) {
      vc->params->model_details.dangles = 2;   /* recompute with dangles as in pf_fold() */
      min_en                            = vrna_eval_structure(vc, mfe_structure);
      vc->params->model_details.dangles = 1;
    }

    vrna_exp_params_rescale(vc, &min_en);
    kT = vc->exp_params->kT / 1000.;

    if (n > 2000)
      vrna_cstr_message_info(o_stream->err,
                             "scaling factor %f",
                             vc->exp_params->pf_scale);

    /* compute partition function */
    dG = vrna_pf_multimer(vc, pairing_propensity);

    if (opt->md.compute_bpp) {
      char *costruc;
      prAB = vrna_plist_from_probs(vc, opt->bppmThreshold);

      (void)vrna_plot_dp_PS_list(record->sequence,
                                 vc->cutpoint,
                                 "newdot.ps",
                                 prAB,
                                 NULL,
                                 "Multi Probs");

      costruc = vrna_cut_point_insert(pairing_propensity, vc->cutpoint);
      vrna_cstr_printf_structure(o_stream->data,
                                 costruc,
                                 record->tty ? "\n free energy of connected ensemble = %6.2f kcal/mol" : " [%6.2f]",
                                 dG);

      free(costruc);
    } else {
      vrna_cstr_printf_structure(o_stream->data,
                                 NULL,
                                 " free energy of connected ensemble = %g kcal/mol",
                                 dG);
    }

    free(prAB);
    free(pairing_propensity);
  }   /*end if(pf)*/

  if ((!opt->doC) && (opt->csv_output)) /* end of data set in case we output as CSV */
    vrna_cstr_printf(o_stream->data, "\n");

  if (opt->output_queue)
    vrna_ostream_provide(opt->output_queue, record->number, (void *)o_stream);
  else
    flush_cstr_callback(NULL, 0, (void *)o_stream);

  /* clean up */
  free(record->SEQ_ID);
  free(record->id);
  free(sequence);
  free(record->sequence);
  free(mfe_structure);
  /* free the rest of current dataset */
  if (record->rest) {
    for (i = 0; record->rest[i]; i++)
      free(record->rest[i]);
    free(record->rest);
  }

  vrna_fold_compound_free(vc);

  free(record);
}


static void
write_csv_header(FILE           *output,
                 struct options *opt)
{
  vrna_cstr_t stream = vrna_cstr(100, output);

  /* compose header line for CSV output */
  if (opt->pf) {
    if (opt->md.compute_bpp) {
      if (opt->doT) {
        vrna_cstr_printf(stream,
                         "seq_num%c"
                         "seq_id%c"
                         "seq%c"
                         "mfe_struct%c"
                         "mfe%c"
                         "bpp_string%c"
                         "ensemble_energy%c"
                         "AB%c"
                         "AA%c"
                         "BB%c"
                         "A%c"
                         "B\n",
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim);
      } else {
        vrna_cstr_printf(stream,
                         "seq_num%c"
                         "seq_id%c"
                         "seq%c"
                         "mfe_struct%c"
                         "mfe%c",
                         "bpp_string%c"
                         "ensemble_energy\n",
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim,
                         opt->csv_output_delim);
      }
    } else {
      vrna_cstr_printf(stream,
                       "seq_num%c"
                       "seq_id%c"
                       "seq%c"
                       "mfe_struct%c"
                       "mfe%c",
                       "ensemble_energy\n",
                       opt->csv_output_delim,
                       opt->csv_output_delim,
                       opt->csv_output_delim,
                       opt->csv_output_delim,
                       opt->csv_output_delim);
    }
  } else {
    vrna_cstr_printf(stream,
                     "seq_num%c"
                     "seq_id%c"
                     "seq%c"
                     "mfe_struct%c"
                     "mfe\n",
                     opt->csv_output_delim,
                     opt->csv_output_delim,
                     opt->csv_output_delim,
                     opt->csv_output_delim);
  }

  vrna_cstr_fflush(stream);
  vrna_cstr_close(stream);
}


static void
compute_MEA(vrna_fold_compound_t  *fc,
            double                MEAgamma,
            vrna_cstr_t           rec_output)
{
  char  *structure, *mea_structure;
  float mea, mea_en;

  structure = vrna_MEA(fc, MEAgamma, &mea);

  mea_en = vrna_eval_structure(fc, (const char *)structure);

  /* insert cut point */
  mea_structure = vrna_cut_point_insert(structure, fc->cutpoint);

  vrna_cstr_printf_structure(rec_output, mea_structure, " {%6.2f MEA=%.2f}", mea_en, mea);

  free(structure);
  free(mea_structure);
}


static void
compute_centroid(vrna_fold_compound_t *fc,
                 vrna_cstr_t          rec_output)
{
  char    *cent, *centroid_structure;
  double  cent_en, dist;

  cent    = vrna_centroid(fc, &dist);
  cent_en = vrna_eval_structure(fc, (const char *)cent);

  /* insert cut point */
  centroid_structure = vrna_cut_point_insert(cent, fc->cutpoint);

  vrna_cstr_printf_structure(rec_output, centroid_structure, " {%6.2f d=%.2f}", cent_en, dist);

  free(cent);
  free(centroid_structure);
}


static char *
get_filename(const char     *id,
             const char     *suffix,
             const char     *filename_default,
             struct options *opt)
{
  char *tmp_string, *filename = NULL;

  if (id) {
    filename = vrna_strdup_printf("%s%s%s", id, opt->filename_delim, suffix);
    /* sanitize file names */
    tmp_string = vrna_filename_sanitize(filename, opt->filename_delim);
    free(filename);
    filename = tmp_string;
  } else {
    filename = vrna_strdup_printf("%s", filename_default);
  }

  return filename;
}


PRIVATE vrna_dimer_pf_t
do_partfunc(char            *string,
            int             length,
            int             Switch,
            plist           **tpr,
            plist           **mfpl,
            double          kT,
            struct options  *opt)
{
  /*compute mfe and partition function of dimer or monomer*/
  char                  *Newstring;
  char                  *tempstruc;
  double                min_en;
  vrna_md_t             *md;
  vrna_dimer_pf_t       X;
  vrna_fold_compound_t  *vc;

  md = &(opt->md);

  switch (Switch) {
    case 1:   /* monomer */
      tempstruc = (char *)vrna_alloc((unsigned)length + 1);
      vc        = vrna_fold_compound(string,
                                     md,
                                     VRNA_OPTION_MFE | VRNA_OPTION_PF);
      min_en  = vrna_mfe(vc, tempstruc);
      *mfpl   = vrna_plist(tempstruc, 0.95);
      vrna_mx_mfe_free(vc);

      vrna_exp_params_rescale(vc, &min_en);

      X = vrna_pf_dimer(vc, NULL);
      if (md->compute_bpp)
        *tpr = vrna_plist_from_probs(vc, opt->bppmThreshold);

      vrna_fold_compound_free(vc);
      free(tempstruc);
      break;

    case 2:   /* dimer */
      tempstruc = (char *)vrna_alloc((unsigned)length * 2 + 2);
      Newstring = (char *)vrna_alloc(sizeof(char) * (length * 2 + 2));
      strcat(Newstring, string);
      strcat(Newstring, "&");
      strcat(Newstring, string);

      vc = vrna_fold_compound(Newstring,
                              md,
                              VRNA_OPTION_MFE | VRNA_OPTION_PF | VRNA_OPTION_HYBRID);

      min_en  = vrna_mfe_dimer(vc, tempstruc);
      *mfpl   = vrna_plist(tempstruc, 0.95);
      vrna_mx_mfe_free(vc);

      vrna_exp_params_rescale(vc, &min_en);

      X = vrna_pf_dimer(vc, NULL);
      if (md->compute_bpp)
        *tpr = vrna_plist_from_probs(vc, opt->bppmThreshold);

      vrna_fold_compound_free(vc);

      free(Newstring);
      free(tempstruc);
      break;

    default:
      printf("Error in get_partfunc\n, computing neither mono- nor dimere!\n");
      exit(42);
  }

  return X;
}


void
postscript_layout(vrna_fold_compound_t  *fc,
                  const char            *orig_sequence,
                  const char            *structure,
                  const char            *SEQ_ID,
                  struct options        *opt)
{
  char *filename_plot, *annot, *tmp_string;

  filename_plot = NULL;
  annot         = NULL;

  if (SEQ_ID) {
    filename_plot = vrna_strdup_printf("%s%sss.ps", SEQ_ID, opt->filename_delim);
    tmp_string    = vrna_filename_sanitize(filename_plot, opt->filename_delim);
    free(filename_plot);
    filename_plot = tmp_string;
  } else {
    filename_plot = strdup("rna.ps");
  }

  if (fc->cutpoint >= 0) {
    annot = vrna_strdup_printf("1 %d 9  0 0.9 0.2 omark\n"
                               "%d %d 9  1 0.1 0.2 omark\n",
                               fc->cutpoint - 1,
                               fc->cutpoint + 1,
                               fc->length + 1);
  }

  if (filename_plot) {
    THREADSAFE_FILE_OUTPUT(
      (void)vrna_file_PS_rnaplot_a(orig_sequence,
                                   structure,
                                   filename_plot,
                                   annot,
                                   NULL,
                                   &(opt->md)));
  }

  free(filename_plot);
  free(annot);
}


PRIVATE void
print_concentrations(vrna_cstr_t        stream,
                     vrna_dimer_conc_t  *result,
                     double             *startconc)
{
  /* compute and print concentrations out of free energies, calls get_concentrations */
  int i, n;

  vrna_cstr_printf_thead(stream,
                         "Initial concentrations\t\trelative Equilibrium concentrations\n"
                         "A\t\tB\t\tAB\t\tAA\t\tBB\t\tA\t\tB");

  for (n = 0; (startconc[2 * n] > 0) || (startconc[2 * n + 1] > 0); n++); /* count */
  for (i = 0; i < n; i++) {
    double tot = result[i].Ac_start + result[i].Bc_start;
    vrna_cstr_printf_tbody(stream,
                           "%-10g\t%-10g\t%.5f \t%.5f \t%.5f \t%.5f \t%.5f",
                           result[i].Ac_start,
                           result[i].Bc_start,
                           result[i].ABc / tot,
                           result[i].AAc / tot,
                           result[i].BBc / tot,
                           result[i].Ac / tot,
                           result[i].Bc / tot);
  }
}


PRIVATE double *
read_concentrations(FILE *fp)
{
  /* reads concentrations, returns list of double, -1. marks end */
  char    *line;
  double  *startc;
  int     i = 0, n = 2;

  startc = (double *)vrna_alloc((2 * n + 1) * sizeof(double));

  while ((line = vrna_read_line(fp)) != NULL) {
    int c;
    if (i + 4 >= 2 * n) {
      n       *= 2;
      startc  = (double *)vrna_realloc(startc, (2 * n + 1) * sizeof(double));
    }

    c = sscanf(line, "%lf %lf", &startc[i], &startc[i + 1]);
    free(line);
    if (c < 2)
      break;

    i += 2;
  }
  startc[i] = startc[i + 1] = 0;
  return startc;
}