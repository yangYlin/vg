#include "subcommand.hpp"
#include "../vg.hpp"
#include "../utility.hpp"
#include "../packer.hpp"
#include <vg/io/stream.hpp>
#include <vg/io/vpkg.hpp>

#include <unistd.h>
#include <getopt.h>

using namespace vg;
using namespace vg::subcommand;

void help_pack(char** argv) {
    cerr << "usage: " << argv[0] << " pack [options]" << endl
         << "options:" << endl
         << "    -x, --xg FILE          use this basis graph" << endl
         << "    -o, --packs-out FILE   write compressed coverage packs to this output file" << endl
         << "    -i, --packs-in FILE    begin by summing coverage packs from each provided FILE" << endl
         << "    -g, --gam FILE         read alignments from this file (could be '-' for stdin)" << endl
         << "    -d, --as-table         write table on stdout representing packs" << endl
         << "    -e, --with-edits       record and write edits rather than only recording graph-matching coverage" << endl
         << "    -b, --bin-size N       number of sequence bases per CSA bin [default: inf]" << endl
         << "    -n, --node ID          write table for only specified node(s)" << endl
         << "    -N, --node-list FILE   a white space or line delimited list of nodes to collect" << endl
         << "    -t, --threads N        use N threads (defaults to numCPUs)" << endl;
}

int main_pack(int argc, char** argv) {

    string xg_name;
    vector<string> packs_in;
    string packs_out;
    string gam_in;
    bool write_table = false;
    int thread_count = 1;
    bool record_edits = false;
    size_t bin_size = 0;
    vector<vg::id_t> node_ids;
    string node_list_file;

    if (argc == 2) {
        help_pack(argv);
        return 1;
    }

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =
        {
            {"help", no_argument, 0, 'h'},
            {"xg", required_argument,0, 'x'},
            {"packs-out", required_argument,0, 'o'},
            {"count-in", required_argument, 0, 'i'},
            {"gam", required_argument, 0, 'g'},
            {"as-table", no_argument, 0, 'd'},
            {"threads", required_argument, 0, 't'},
            {"with-edits", no_argument, 0, 'e'},
            {"node", required_argument, 0, 'n'},
            {"node-list", required_argument, 0, 'N'},
            {"bin-size", required_argument, 0, 'b'},
            {0, 0, 0, 0}

        };
        int option_index = 0;
        c = getopt_long (argc, argv, "hx:o:i:g:dt:eb:n:N:",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {

        case '?':
        case 'h':
            help_pack(argv);
            return 1;
        case 'x':
            xg_name = optarg;
            break;
        case 'o':
            packs_out = optarg;
            break;
        case 'i':
            packs_in.push_back(optarg);
            break;
        case 'g':
            gam_in = optarg;
            break;
        case 'd':
            write_table = true;
            break;
        case 'e':
            record_edits = true;
            break;
        case 'b':
            bin_size = atoll(optarg);
            break;
        case 't':
            thread_count = parse<int>(optarg);
            break;
        case 'n':
            node_ids.push_back(parse<int>(optarg));
            break;
        case 'N':
            node_list_file = optarg;
            break;

        default:
            abort();
        }
    }

    omp_set_num_threads(thread_count);

    unique_ptr<xg::XG> xgidx;
    if (xg_name.empty()) {
        cerr << "No XG index given. An XG index must be provided." << endl;
        exit(1);
    } else {
        xgidx = vg::io::VPKG::load_one<xg::XG>(xg_name);
    }

    // process input node list
    if (!node_list_file.empty()) {
        ifstream nli;
        nli.open(node_list_file);
        if (!nli.good()){
            cerr << "[vg pack] error, unable to open the node list input file." << endl;
            exit(1);
        }
        string line;
        while (getline(nli, line)){
            for (auto& idstr : split_delims(line, " \t")) {
                node_ids.push_back(parse<int64_t>(idstr.c_str()));
            }
        }
        nli.close();
    }

    // todo one packer per thread and merge

    vg::Packer packer(xgidx.get(), bin_size);
    if (packs_in.size() == 1) {
        packer.load_from_file(packs_in.front());
    } else if (packs_in.size() > 1) {
        packer.merge_from_files(packs_in);
    }

    if (!gam_in.empty()) {
        vector<vg::Packer*> packers;
        if (thread_count == 1) {
            packers.push_back(&packer);
        } else {
            for (size_t i = 0; i < thread_count; ++i) {
                packers.push_back(new Packer(xgidx.get(), bin_size));
            }
        }
        std::function<void(Alignment&)> lambda = [&packer,&record_edits,&packers](Alignment& aln) {
            packers[omp_get_thread_num()]->add(aln, record_edits);
        };
        if (gam_in == "-") {
            vg::io::for_each_parallel(std::cin, lambda);
        } else {
            ifstream gam_stream(gam_in);
            vg::io::for_each_parallel(gam_stream, lambda);
            gam_stream.close();
        }
        if (thread_count == 1) {
            packers.clear();
        } else {
            packer.merge_from_dynamic(packers);
            for (auto& p : packers) {
                delete p;
            }
            packers.clear();
        }
    }

    if (!packs_out.empty()) {
        packer.save_to_file(packs_out);
    }
    if (write_table) {
        packer.make_compact();
        packer.as_table(cout, record_edits, node_ids);
    }

    return 0;
}

// Register subcommand
static Subcommand vg_pack("pack", "convert alignments to a compact coverage, edit, and path index", main_pack);
