#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <map>
#include <pthread.h>
#include <unistd.h>
#include "mvt.hpp"
#include "plugin.hpp"
#include "write_json.hpp"
#include "projection.hpp"
#include "geometry.hpp"

extern "C" {
#include "jsonpull/jsonpull.h"
}

struct writer_arg {
	int *pipe_orig;
	mvt_layer *layer;
	unsigned z;
	unsigned x;
	unsigned y;
};

void *run_writer(void *a) {
	writer_arg *wa = (writer_arg *) a;

	// XXX worry about SIGPIPE?

	FILE *fp = fdopen(wa->pipe_orig[1], "w");
	if (fp == NULL) {
		perror("fdopen (pipe writer)");
		exit(EXIT_FAILURE);
	}

	layer_to_geojson(fp, *(wa->layer), wa->z, wa->x, wa->y);

	if (fclose(fp) != 0) {
		perror("fclose output to filter");
		exit(EXIT_FAILURE);
	}

	return NULL;
}

static void json_context(json_object *j) {  // XXX share with geojson.cpp
	char *s = json_stringify(j);

	if (strlen(s) >= 500) {
		sprintf(s + 497, "...");
	}

	fprintf(stderr, "In JSON object %s\n", s);
	free(s);  // stringify
}

mvt_layer parse_layer(int fd, unsigned z, unsigned x, unsigned y) {
	mvt_layer ret;

	FILE *f = fdopen(fd, "r");
	if (f == NULL) {
		perror("fdopen filter output");
		exit(EXIT_FAILURE);
	}
	json_pull *jp = json_begin_file(f);

	while (1) {
		json_object *j = json_read(jp);
		if (j == NULL) {
			if (jp->error != NULL) {
				fprintf(stderr, "Filter output:%d: %s\n", jp->line, jp->error);
				if (jp->root != NULL) {
					json_context(jp->root);
				}
				exit(EXIT_FAILURE);
			}

			json_free(jp->root);
			break;
		}

		json_object *type = json_hash_get(j, "type");
		if (type == NULL || type->type != JSON_STRING) {
			continue;
		}
		if (strcmp(type->string, "Feature") != 0) {
			continue;
		}

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == NULL) {
			fprintf(stderr, "Filter output:%d: filtered feature with no geometry\n", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_FAILURE);
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == NULL || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "Filter output:%d: feature without properties hash\n", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_FAILURE);
		}

		json_object *geometry_type = json_hash_get(geometry, "type");
		if (geometry_type == NULL) {
			fprintf(stderr, "Filter output:%d: null geometry (additional not reported)\n", jp->line);
			json_context(j);
			exit(EXIT_FAILURE);
		}

		if (geometry_type->type != JSON_STRING) {
			fprintf(stderr, "Filter output:%d: geometry type is not a string\n", jp->line);
			json_context(j);
			exit(EXIT_FAILURE);
		}

		json_object *coordinates = json_hash_get(geometry, "coordinates");
		if (coordinates == NULL || coordinates->type != JSON_ARRAY) {
			fprintf(stderr, "Filter output:%d: feature without coordinates array\n", jp->line);
			json_context(j);
			exit(EXIT_FAILURE);
		}

#if 0
		int t;
		for (t = 0; t < GEOM_TYPES; t++) {
			if (strcmp(geometry_type->string, geometry_names[t]) == 0) {
				break;
			}
		}
		if (t >= GEOM_TYPES) {
			fprintf(stderr, "Filter output:%d: Can't handle geometry type %s\n", jp->line, geometry_type->string);
			json_context(j);
			exit(EXIT_FAILURE);
		}
#endif

		json_free(j);
	}

	json_end(jp);
	return ret;
}

mvt_layer filter_layer(const char *filter, mvt_layer &layer, unsigned z, unsigned x, unsigned y) {
	// This will create two pipes, a new thread, and a new process.
	//
	// The new process will read from one pipe and write to the other, and execute the filter.
	// The new thread will write the GeoJSON to the pipe that leads to the filter.
	// The original thread will read the GeoJSON from the filter and convert it back into vector tiles.

	int pipe_orig[2], pipe_filtered[2];
	if (pipe(pipe_orig) < 0) {
		perror("pipe (original features)");
		exit(EXIT_FAILURE);
	}
	if (pipe(pipe_filtered) < 0) {
		perror("pipe (filtered features)");
		exit(EXIT_FAILURE);
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		// child

		if (dup2(pipe_orig[0], 0) < 0) {
			perror("dup child stdin");
			exit(EXIT_FAILURE);
		}
		if (dup2(pipe_filtered[1], 1) < 0) {
			perror("dup child stdout");
			exit(EXIT_FAILURE);
		}
		if (close(pipe_orig[1]) != 0) {
			perror("close output to filter");
			exit(EXIT_FAILURE);
		}
		if (close(pipe_filtered[0]) != 0) {
			perror("close input from filter");
			exit(EXIT_FAILURE);
		}

		// XXX close other fds?

		// XXX add zyx args
		if (execlp("sh", "sh", "-c", filter, NULL) != 0) {
			perror("exec");
			exit(EXIT_FAILURE);
		}
	} else {
		// parent

		if (close(pipe_orig[0]) != 0) {
			perror("close filter-side reader");
			exit(EXIT_FAILURE);
		}
		if (close(pipe_filtered[1]) != 0) {
			perror("close filter-side writer");
			exit(EXIT_FAILURE);
		}

		writer_arg wa;
		wa.pipe_orig = pipe_orig;
		wa.layer = &layer;
		wa.z = z;
		wa.x = x;
		wa.y = y;

		pthread_t writer;
		if (pthread_create(&writer, NULL, run_writer, &wa) != 0) {
			perror("pthread_create (filter writer)");
			exit(EXIT_FAILURE);
		}

		layer = parse_layer(pipe_filtered[0], z, x, y);

		int stat_loc;
		if (waitpid(pid, &stat_loc, 0) < 0) {
			perror("waitpid for filter\n");
			exit(EXIT_FAILURE);
		}

		if (close(pipe_filtered[0]) != 0) {
			perror("close output from filter");
			exit(EXIT_FAILURE);
		}

		void *ret;
		if (pthread_join(writer, &ret) != 0) {
			perror("pthread_join filter writer");
			exit(EXIT_FAILURE);
		}
	}

	return layer;
}
